//
// test_soupbin.cpp — SoupBinTCP 3.00: framing, login, heartbeats, timeouts.
//
// There is no real SoupBinTCP session anywhere in this project's future — see
// messages.hpp's banner. What backstops correctness here is the design review
// that ran BEFORE any of session.hpp existed, and this file exists to prove
// each finding from that review actually got fixed rather than just written
// about. Every test below traces to a specific finding; the comment on each
// says which.
//
// Every on_bytes()/send_*() call below passes an explicit `now_ns`, on
// purpose: the first draft of session.hpp cached "now" from whichever tick()
// last ran and reused it inside on_bytes(), which reset the receive-idle
// clock to a STALE timestamp — zero, if tick() had never yet been called.
// test_clock_backward_safety() is the test that caught it, by ticking with a
// deliberately backward clock and finding the session went Dead on the very
// next call regardless. Passing `now` explicitly everywhere is the fix, and
// this file exercises that every call site actually does.
//
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/soupbin/encode.hpp"
#include "itchbook/soupbin/messages.hpp"
#include "itchbook/soupbin/session.hpp"
#include "tests/check.hpp"

namespace {

namespace s = itchbook::soupbin;
namespace e = itchbook::soupbin::encode;

class VecSink : public s::Sink {
public:
    std::vector<uint8_t> out;
    std::vector<std::vector<uint8_t>> messages;   // one entry per on_message call
    void on_message(const uint8_t* p, size_t n) override {
        out.insert(out.end(), p, p + n);
        messages.emplace_back(p, p + n);
    }
    void clear() { out.clear(); messages.clear(); }
};

constexpr uint64_t SEC = 1'000'000'000ULL;

s::ClientSession::Config client_cfg() {
    s::ClientSession::Config c;
    c.server_dead_threshold_ns = 15 * SEC;
    c.login_response_timeout_ns = 30 * SEC;
    return c;
}

// ---- 1. wire framing, every packet type ---------------------------------------
void test_framing_round_trip() {
    uint8_t b[128];
    size_t n;

    n = e::login_request(b, "USER01", "PASSWORD1", "SESSABC12", "9999999999999999999");
    CHECK_EQ(n, s::kLoginRequestWireBytes);
    CHECK_EQ(s::frame_length(b), n);
    CHECK_EQ(s::packet_type(b), 'L');

    n = e::login_accepted(b, "SESS1", "42");
    CHECK_EQ(n, s::kLoginAcceptedWireBytes);
    CHECK_EQ(s::packet_type(b), 'A');
    CHECK(std::memcmp(s::login_accepted::session(b), "     SESS1", 10) == 0);

    n = e::login_rejected(b, 'S');
    CHECK_EQ(n, s::kLoginRejectedWireBytes);
    CHECK_EQ(s::login_rejected::reason(b), 'S');

    const uint8_t msg[] = {1, 2, 3, 4, 5};
    n = e::sequenced_data(b, msg, sizeof(msg));
    CHECK_EQ(n, size_t{3 + 5});
    CHECK_EQ(s::sequenced_data::message_length(b), size_t{5});
    CHECK(std::memcmp(s::sequenced_data::message(b), msg, 5) == 0);

    n = e::server_heartbeat(b);
    CHECK_EQ(n, s::kServerHeartbeatWireBytes);
    n = e::end_of_session(b);
    CHECK_EQ(n, s::kEndOfSessionWireBytes);
    n = e::unsequenced_data(b, msg, sizeof(msg));
    CHECK_EQ(n, size_t{3 + 5});
    n = e::client_heartbeat(b);
    CHECK_EQ(n, s::kClientHeartbeatWireBytes);
    n = e::logout_request(b);
    CHECK_EQ(n, s::kLogoutRequestWireBytes);

    // Right-padding (Username/Password) vs left-padding (Session/Sequence
    // Number) are two different rules for two different field groups, per
    // messages.hpp's padding_directions evidence -- checked explicitly so a
    // future edit that reaches for the wrong helper on the wrong field fails
    // here rather than corrupting a login silently.
    n = e::login_request(b, "AB", "CD", "EF", "1");
    CHECK(std::memcmp(s::login_request::username(b), "AB    ", 6) == 0);       // right
    CHECK(std::memcmp(s::login_request::password(b), "CD        ", 10) == 0);  // right
    CHECK(std::memcmp(s::login_request::requested_session(b), "        EF", 10) == 0);  // left
}

// ---- 2. every possible split point of a real message ---------------------------
//
// framing lens, blocker #1 and #2: a naive parser reads out of bounds when a
// read ends inside the 2-byte length field, and under-drains a batch of
// complete frames plus a trailing partial. This delivers a real 33-byte
// Login Accepted packet split at EVERY possible byte boundary from 1 to 32,
// one on_bytes() call per fragment, and requires that dispatch fires exactly
// once, only once the full frame is present -- never early, never missed.
void test_byte_at_every_split_point() {
    uint8_t frame[s::kLoginAcceptedWireBytes];
    e::login_accepted(frame, "SESS1", "1");

    for (size_t split = 1; split < s::kLoginAcceptedWireBytes; ++split) {
        VecSink out;
        s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
        out.clear();   // drop the constructor's own Login Request
        c.on_bytes(frame, split, 1);
        CHECK(c.state() == s::State::AwaitingLogin);   // not yet -- partial
        c.on_bytes(frame + split, s::kLoginAcceptedWireBytes - split, 2);
        CHECK(c.state() == s::State::LoggedIn);        // now, exactly
    }

    // And truly one byte at a time, the whole way.
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    for (size_t i = 0; i < s::kLoginAcceptedWireBytes; ++i) {
        c.on_bytes(frame + i, 1, static_cast<uint64_t>(i) + 1);
        if (i + 1 < s::kLoginAcceptedWireBytes) {
            CHECK(c.state() == s::State::AwaitingLogin);
        }
    }
    CHECK(c.state() == s::State::LoggedIn);
}

// ---- 3. several complete frames + a terminal one, coalesced in ONE call --------
//
// framing lens, blocker #2's own named scenario: the graceful-shutdown case
// where a peer's last packets and its End of Session arrive in a single TCP
// read right before the socket closes, so there is no second on_bytes()
// call to drain a straggler.
void test_multiple_frames_plus_terminal_in_one_call() {
    VecSink cout, sout, app;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, &app);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 1);
    CHECK(c.state() == s::State::LoggedIn);

    std::vector<uint8_t> batch;
    for (int i = 0; i < 3; ++i) {
        uint8_t msg[3] = {uint8_t(i), 0, 0};
        uint8_t f[6];
        const size_t n = e::sequenced_data(f, msg, 3);
        batch.insert(batch.end(), f, f + n);
    }
    uint8_t z[s::kEndOfSessionWireBytes];
    const size_t zn = e::end_of_session(z);
    batch.insert(batch.end(), z, z + zn);

    const bool changed = c.on_bytes(batch.data(), batch.size(), 2);
    CHECK(changed);
    CHECK(c.state() == s::State::Ended);
    CHECK_EQ(app.messages.size(), size_t{3});   // all three Sequenced Data dispatched
    CHECK_EQ(c.received_sequence_number(), uint64_t{3});
}

// ---- 4. clock non-monotonicity never spuriously trips or suppresses -----------
//
// heartbeat_timeout lens, blocker #1.
void test_clock_backward_safety() {
    VecSink out;
    s::ClientSession c(client_cfg(), 1000 * SEC, "U", "P", "", "1", &out, nullptr);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 1000 * SEC);
    CHECK(c.state() == s::State::LoggedIn);

    // Feed a NOW that goes backward relative to the session's own start
    // time, repeatedly, well past what a naive unsigned-subtraction bug
    // would need to wrap.
    for (uint64_t back = 0; back < 5; ++back) {
        const bool changed = c.tick(500 * SEC);   // before start_now_ns
        CHECK(!changed);
        CHECK(c.state() == s::State::LoggedIn);
    }
    // Then genuinely advance and confirm detection still works normally --
    // proving the guard does not ALSO suppress real timeouts forever.
    c.tick(1000 * SEC + 20 * SEC);
    CHECK(c.state() == s::State::Dead);
}

// ---- 5. zero-length on_bytes() is not activity ---------------------------------
//
// framing lens, major finding on on_bytes(p, 0) semantics.
void test_zero_length_bytes_not_activity() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 0);

    uint64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        now += SEC;
        c.on_bytes(nullptr, 0, now);     // must NOT reset the receive-idle clock
        c.on_bytes(la, 0, now);          // neither must a non-null, zero-length call
        c.tick(now);
    }
    CHECK_EQ(now, 20 * SEC);
    CHECK(c.state() == s::State::Dead);   // 20s of true silence > 15s threshold
}

// ---- 6. heartbeats hold an idle session -- the phase's own done-condition -----
void test_heartbeats_hold_idle_session() {
    VecSink cout, sout;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, nullptr);
    s::ServerSession srv({}, 0, &sout, nullptr);
    srv.on_bytes(cout.out.data(), cout.out.size(), 0);
    sout.clear();
    srv.accept_login("SESS1", "1", 0);
    c.on_bytes(sout.out.data(), sout.out.size(), 0);
    CHECK(c.state() == s::State::LoggedIn);
    CHECK(srv.state() == s::State::LoggedIn);

    uint64_t now = 0;
    // 300 ticks at an irregular 0.37s step -- deliberately not aligned to
    // the 1s heartbeat boundary, per the review's concern about irregular
    // polling cadence -- covering >100 simulated seconds, an order of
    // magnitude past the 15s dead-peer threshold, with nothing but
    // heartbeats flowing.
    for (int i = 0; i < 300; ++i) {
        now += 370'000'000ULL;
        cout.clear();
        sout.clear();
        c.tick(now);
        srv.tick(now);
        if (!cout.out.empty()) srv.on_bytes(cout.out.data(), cout.out.size(), now);
        if (!sout.out.empty()) c.on_bytes(sout.out.data(), sout.out.size(), now);
    }
    CHECK(c.state() == s::State::LoggedIn);
    CHECK(srv.state() == s::State::LoggedIn);
}

// ---- 7. dead peer detected within the timeout, not before, exactly once -------
void test_dead_peer_detected_within_timeout() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 0);
    CHECK(c.state() == s::State::LoggedIn);

    uint64_t now = 0;
    for (; now < 15 * SEC - SEC / 2; now += SEC / 2) {
        const bool changed = c.tick(now);
        CHECK(!changed);
    }
    CHECK(c.state() == s::State::LoggedIn);   // still alive just under 15s

    now = 15 * SEC + 1;
    const bool edge = c.tick(now);
    CHECK(edge);
    CHECK(c.state() == s::State::Dead);

    const bool edge2 = c.tick(now + SEC);      // same Dead state, later tick
    CHECK(!edge2);                             // edge fires exactly once
    CHECK(c.state() == s::State::Dead);
}

// ---- 8. LoginTimedOut is distinct from Dead -------------------------------------
//
// interface lens, major finding: "never got in" vs "was alive and went dark"
// must be told apart.
void test_login_timed_out_distinct_from_dead() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    CHECK(c.state() == s::State::AwaitingLogin);
    const bool changed = c.tick(30 * SEC + 1);
    CHECK(changed);
    CHECK(c.state() == s::State::LoginTimedOut);
    CHECK(c.state() != s::State::Dead);

    VecSink sout;
    s::ServerSession srv({}, 0, &sout, nullptr);
    CHECK(srv.state() == s::State::AwaitingLogin);
    const bool schanged = srv.tick(30 * SEC + 1);
    CHECK(schanged);
    CHECK(srv.state() == s::State::LoginTimedOut);
}

// ---- 9. the 15s/30s conflict: exactly one timeout applies per phase -----------
//
// heartbeat_timeout lens, blocker #2: a shorter general-purpose timeout must
// not silently pre-empt the login-specific one. Server's AwaitingLogin uses
// ONLY login_timeout_ns (30s default); nothing fires before then even though
// 15s (the LoggedIn-only client_dead_threshold_ns) has long since elapsed.
void test_only_login_timeout_applies_pre_login() {
    VecSink sout;
    s::ServerSession srv({}, 0, &sout, nullptr);
    for (uint64_t now = SEC; now < 29 * SEC; now += SEC) {
        srv.tick(now);
        CHECK(srv.state() == s::State::AwaitingLogin);
    }
    // Past 15s (the LoggedIn threshold) but well under 30s: still waiting.
    CHECK(srv.state() == s::State::AwaitingLogin);
    srv.tick(30 * SEC + 1);
    CHECK(srv.state() == s::State::LoginTimedOut);
}

// ---- 10. LoginReceived has no timeout of its own --------------------------------
void test_login_received_has_no_timeout() {
    VecSink cout, sout;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, nullptr);
    s::ServerSession srv({}, 0, &sout, nullptr);
    srv.on_bytes(cout.out.data(), cout.out.size(), 0);
    CHECK(srv.state() == s::State::LoginReceived);

    // Advance far past every configured timeout while the application
    // simply hasn't decided accept/reject yet -- must not time out, because
    // the spec gives no number for this wait and inventing one would be
    // exactly the misattribution the design deliberately avoids elsewhere.
    for (uint64_t now = SEC; now < 120 * SEC; now += 10 * SEC) {
        srv.tick(now);
    }
    CHECK(srv.state() == s::State::LoginReceived);
}

// ---- 11. malformed framing -> ProtocolViolation, not Dead, not silence --------
//
// interface lens, blocker #1: distinguishable from silence.
void test_protocol_violation_wrong_fixed_length() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    // A Server Heartbeat ('H') must be exactly 3 wire bytes; hand-craft one
    // claiming an extra byte of payload it does not have room for.
    uint8_t bad[4] = {0, 2, 'H', 0};   // declared_length=2, so type+1 extra byte
    const bool changed = c.on_bytes(bad, sizeof(bad), 1);
    CHECK(changed);
    CHECK(c.state() == s::State::ProtocolViolation);
}

void test_protocol_violation_unknown_type() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 0);
    CHECK(c.state() == s::State::LoggedIn);

    uint8_t bad[3] = {0, 1, '?'};   // '?' is not a type a client ever receives
    const bool changed = c.on_bytes(bad, sizeof(bad), 1);
    CHECK(changed);
    CHECK(c.state() == s::State::ProtocolViolation);
}

void test_protocol_violation_oversized_declared_length() {
    s::ClientSession::Config cfg = client_cfg();
    cfg.max_frame_bytes = 64;
    VecSink out;
    s::ClientSession c(cfg, 0, "U", "P", "", "1", &out, nullptr);
    uint8_t bad[3];
    bad[0] = static_cast<uint8_t>(60000 >> 8);
    bad[1] = static_cast<uint8_t>(60000);
    bad[2] = 'S';   // Sequenced Data, variable-length, would exceed max_frame_bytes
    const bool changed = c.on_bytes(bad, sizeof(bad), 1);
    CHECK(changed);
    CHECK(c.state() == s::State::ProtocolViolation);
}

// A well-FRAMED packet that makes no sense in the current state is just as
// much a protocol violation as a garbled length -- extends beyond what the
// review literally asked for, closing a gap found while implementing it: an
// unexpected second Login Accepted must not be silently swallowed.
void test_protocol_violation_out_of_state() {
    VecSink out;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &out, nullptr);
    uint8_t la[s::kLoginAcceptedWireBytes];
    e::login_accepted(la, "SESS1", "1");
    c.on_bytes(la, sizeof(la), 0);
    CHECK(c.state() == s::State::LoggedIn);

    const bool changed = c.on_bytes(la, sizeof(la), 1);   // a SECOND Login Accepted
    CHECK(changed);
    CHECK(c.state() == s::State::ProtocolViolation);
}

// ---- 12. terminal states latch: nothing overwrites a graceful close -----------
//
// heartbeat_timeout lens, minor finding: Ended must not later become Dead.
void test_terminal_state_latched() {
    VecSink cout, sout;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, nullptr);
    s::ServerSession srv({}, 0, &sout, nullptr);
    srv.on_bytes(cout.out.data(), cout.out.size(), 0);
    sout.clear();
    srv.accept_login("SESS1", "1", 0);
    c.on_bytes(sout.out.data(), sout.out.size(), 0);

    sout.clear();
    srv.send_end_of_session();
    c.on_bytes(sout.out.data(), sout.out.size(), 0);
    CHECK(c.state() == s::State::Ended);

    // Advance far past every timeout: Ended must not become Dead.
    for (uint64_t now = SEC; now < 100 * SEC; now += 5 * SEC) {
        const bool changed = c.tick(now);
        CHECK(!changed);
        CHECK(c.state() == s::State::Ended);
    }

    // Bytes arriving after a terminal state (a straggler on the wire) must
    // not resurrect the session into some other state either.
    uint8_t hb[s::kServerHeartbeatWireBytes];
    e::server_heartbeat(hb);
    const bool bytes_changed = c.on_bytes(hb, sizeof(hb), 200 * SEC);
    CHECK(!bytes_changed);
    CHECK(c.state() == s::State::Ended);
}

// ---- 13. reject reason is exposed, not just a bare enum tag --------------------
//
// interface lens, minor finding, mirroring ouch::rejected::reason()'s own
// precedent in this codebase.
void test_reject_reason_exposed() {
    for (char reason : {'A', 'S'}) {
        VecSink cout, sout;
        s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, nullptr);
        s::ServerSession srv({}, 0, &sout, nullptr);
        srv.on_bytes(cout.out.data(), cout.out.size(), 0);
        sout.clear();
        srv.reject_login(reason);
        CHECK(srv.state() == s::State::Rejected);
        c.on_bytes(sout.out.data(), sout.out.size(), 0);
        CHECK(c.state() == s::State::Rejected);
        CHECK_EQ(c.reject_reason(), reason);
    }
}

// ---- 14. the asymmetric heartbeat-send gating, both halves --------------------
//
// The file banner's derived (not spec-literal-for-both) asymmetry: server
// must not heartbeat before Login Accepted/Rejected is sent; client heart-
// beats unconditionally from construction.
void test_server_heartbeat_gated_until_logged_in() {
    VecSink sout;
    s::ServerSession srv({}, 0, &sout, nullptr);
    for (uint64_t now = SEC; now < 10 * SEC; now += SEC) {
        sout.clear();
        srv.tick(now);
        CHECK(sout.out.empty());   // never a Server Heartbeat pre-login
    }
    // A Login Request arrives, but the application hasn't decided yet.
    uint8_t lr[s::kLoginRequestWireBytes];
    e::login_request(lr, "U", "P", "", "1");
    srv.on_bytes(lr, sizeof(lr), 10 * SEC);
    CHECK(srv.state() == s::State::LoginReceived);
    for (uint64_t now = 10 * SEC; now < 15 * SEC; now += SEC) {
        sout.clear();
        srv.tick(now);
        CHECK(sout.out.empty());   // still nothing -- LoginReceived is also gated
    }
    sout.clear();
    srv.accept_login("SESS1", "1", 15 * SEC);
    CHECK_EQ(sout.messages.size(), size_t{1});   // the Login Accepted itself
    sout.clear();
    srv.tick(16 * SEC + 1);
    CHECK_EQ(sout.messages.size(), size_t{1});   // now the heartbeat obligation applies
    CHECK_EQ(s::packet_type(sout.messages[0].data()), 'H');
}

void test_client_heartbeat_unconditional_from_construction() {
    VecSink cout;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, nullptr);
    cout.clear();
    c.tick(SEC + 1);   // still AwaitingLogin
    CHECK(c.state() == s::State::AwaitingLogin);
    CHECK_EQ(cout.messages.size(), size_t{1});
    CHECK_EQ(s::packet_type(cout.messages[0].data()), 'R');
}

// ---- 15. Sequenced/Unsequenced Data carry the higher-level (OUCH) payload -----
//
// The gap found while implementing: dispatch()'s first draft counted
// Sequenced Data packets but never surfaced their contents anywhere, which
// silently defeated the entire reason the packet type exists. Proven here as
// a full round trip: bytes that look like an OUCH message pass through
// ServerSession::send_sequenced -> the wire -> ClientSession::on_bytes and
// come out the other side byte-identical, and the reverse direction too.
void test_application_payload_round_trips_both_directions() {
    VecSink cout, sout, capp, sapp;
    s::ClientSession c(client_cfg(), 0, "U", "P", "", "1", &cout, &capp);
    s::ServerSession srv({}, 0, &sout, &sapp);
    srv.on_bytes(cout.out.data(), cout.out.size(), 0);
    sout.clear();
    srv.accept_login("SESS1", "1", 0);
    c.on_bytes(sout.out.data(), sout.out.size(), 0);
    CHECK(c.state() == s::State::LoggedIn);

    const uint8_t ouch_like[] = {'A', 1, 2, 3, 4, 5, 6, 7, 8};   // stand-in payload
    sout.clear();
    srv.send_sequenced(ouch_like, sizeof(ouch_like), 1);
    c.on_bytes(sout.out.data(), sout.out.size(), 1);
    CHECK_EQ(capp.messages.size(), size_t{1});
    CHECK_EQ(capp.messages[0].size(), sizeof(ouch_like));
    CHECK(std::memcmp(capp.messages[0].data(), ouch_like, sizeof(ouch_like)) == 0);
    CHECK_EQ(c.received_sequence_number(), uint64_t{1});

    const uint8_t reply[] = {'X', 9, 9};
    cout.clear();
    c.send_unsequenced(reply, sizeof(reply), 2);
    srv.on_bytes(cout.out.data(), cout.out.size(), 2);
    CHECK_EQ(sapp.messages.size(), size_t{1});
    CHECK(std::memcmp(sapp.messages[0].data(), reply, sizeof(reply)) == 0);
}

}  // namespace

int main() {
    test_framing_round_trip();
    test_byte_at_every_split_point();
    test_multiple_frames_plus_terminal_in_one_call();
    test_clock_backward_safety();
    test_zero_length_bytes_not_activity();
    test_heartbeats_hold_idle_session();
    test_dead_peer_detected_within_timeout();
    test_login_timed_out_distinct_from_dead();
    test_only_login_timeout_applies_pre_login();
    test_login_received_has_no_timeout();
    test_protocol_violation_wrong_fixed_length();
    test_protocol_violation_unknown_type();
    test_protocol_violation_oversized_declared_length();
    test_protocol_violation_out_of_state();
    test_terminal_state_latched();
    test_reject_reason_exposed();
    test_server_heartbeat_gated_until_logged_in();
    test_client_heartbeat_unconditional_from_construction();
    test_application_payload_round_trips_both_directions();
    return REPORT();
}
