#pragma once
//
// session.hpp — the SoupBinTCP session state machine: login, heartbeats,
// framing, and the one thing this file exists for, dead-peer detection.
//
// TRANSPORT-AGNOSTIC BY DESIGN. Neither class here opens a socket, reads a
// socket, or calls a clock. Bytes arrive via on_bytes(), outbound bytes leave
// via a caller-supplied Sink (below, structurally the same shape as
// emit::Sink from phase 12.2 — a virtual on_message(), a different namespace
// on purpose, matching itch/ouch/emit's own precedent of not sharing a
// wire-format module's helpers across protocols), and time is DRIVEN by the
// caller calling tick(now_ns), never read internally. That is what makes the
// exhaustive timing tests below possible without a single real wall-clock
// second elapsing, and it is what makes a real socket adapter (built once
// 12.5/12.7 exist) a thin wrapper rather than something this file has to
// anticipate in detail.
//
// ============================================================================
// A REQUIRED USAGE CONTRACT, not a suggestion: tick() must be called on an
// independent periodic timer, decoupled from socket readability. A peer that
// has genuinely gone silent will, by definition, never make the socket
// readable again — if a caller only calls tick() from inside a "data
// arrived" handler, dead-peer detection can never fire, because the one
// event that is supposed to detect silence is gated behind the opposite of
// silence. This was found by adversarial review of this exact design, twice,
// independently, before a line of it existed, and it is the single most
// important sentence in this file. tick() should be called at an interval
// noticeably finer than the smallest configured timeout — for the 1-second
// heartbeat-send threshold, on the order of 100-250ms is reasonable; the
// exact figure is an engineering tradeoff this file does not mandate.
// ============================================================================
//
// TIMEOUT EVIDENCE CLASS — read before trusting a number below. The spec
// states, in section 1.3, exactly one hard number for dead-peer detection:
// the server may treat a silent client as dead after "typically 15 seconds."
// That word is doing real work — it is offered as a recommendation, not a
// protocol minimum, and it is the ONLY one of the four timeout ideas in this
// file the spec gives any number for at all:
//
//   heartbeat-send threshold (both directions)   1 second,  stated as MUST
//   server's client-dead threshold                15 seconds, "typically"
//   server's awaiting-login threshold              30 seconds, "typically"
//   client's server-dead threshold                (no number given, anywhere)
//
// The fourth row is not an oversight in this file — it is a fact about the
// document. Section 1.3 and section 2.2.4 both describe the client's half of
// dead-peer detection only as "an extended period of time," with no
// parenthetical figure either place. Baking in an invented number and
// presenting it as a default would misrepresent an engineering choice as a
// NASDAQ-sourced constant, so ClientSession::Config::server_dead_threshold_ns
// and login_response_timeout_ns have NO default: a caller must choose,
// deliberately, rather than silently inherit a number this document never
// stated.
//
// ONE RULE HERE IS A SYNTHESIS, NOT A LITERAL QUOTE, AND IS LABELLED AS SUCH:
// "any packet, heartbeat included, resets the applicable silence clock." The
// server's own wording keys its rule to "...any data" (sections 1.3, 2.2.4);
// the client's keys to "...anything" / "...no data" (sections 1.3, 2.3.3);
// neither sentence, read alone, settles whether a HEARTBEAT itself satisfies
// its own obligation. What settles it is the version 2.0 revision history:
// "Server and client are now both guaranteed to send something (either data
// or heartbeat) at least once per second" — heartbeats and data are treated
// as the same kind of event for this purpose. That is a real, sourced
// answer, assembled from three sentences rather than quoted from one, and
// the distinction is recorded here rather than smoothed over.
//
// A GENUINE ASYMMETRY BETWEEN THE TWO ROLES, DERIVED RATHER THAN ASSUMED:
// ServerSession's heartbeat-send obligation does not begin at TCP-accept
// time the way ClientSession's begins at Login-Request-send time. Section
// 2.2.1 states "[Login Accepted] will always be the first non-debug packet
// sent by the server after a successful login request" — a Server Heartbeat
// is not a Debug packet, so sending one before Login Accepted (or Rejected)
// would violate that sentence. ClientSession has no analogous constraint:
// nothing says Login Request must precede a heartbeat in some larger
// ordering sense, only that it is sent immediately on connecting, which it
// already is. So the server's send-clock starts only once login resolves;
// the client's starts at construction. This is not stated as one rule for
// both roles anywhere in the spec — it falls out of combining two different
// sentences about two different packets, which is exactly the kind of
// asymmetry a first draft of this file would have smoothed away by assuming
// symmetry the document does not actually grant.
//
// MAX FRAME SIZE. The spec states plainly that "SoupBinTCP does not define a
// maximum payload length." That is true and it is not a design one can build
// on: an unbounded declared length is unbounded memory growth waiting for a
// malformed or adversarial stream. This session only ever carries OUCH 4.2,
// whose largest message (Replaced, 80 bytes) plus the 3-byte SoupBinTCP
// header is nowhere near a kilobyte, so Config::max_frame_bytes defaults to
// 4096 — generous headroom, an ENGINEERING CHOICE specific to this
// deployment, explicitly not a SoupBinTCP-mandated figure, exactly the same
// footing as the "typically" values above rather than pretending to be one.
//
// PROTOCOL VIOLATION IS NOT ONLY MALFORMED BYTES. A well-framed packet that
// arrives in a state where it makes no sense — a second Login Accepted after
// the session is already LoggedIn, a Sequenced Data packet before login has
// resolved (the spec's own ordering guarantee, section 2.2.1, forbids this
// by construction) — is exactly as much a sign that this side's bookkeeping
// has desynced from the peer's as a garbled length field is. Silently
// dropping it would let a session keep reporting healthy while every
// subsequent byte is interpreted against a false premise; both classes
// therefore check "is this type expected in my CURRENT state," not merely
// "is this type ever legal for my role," before dispatching.
//
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/soupbin/encode.hpp"
#include "itchbook/soupbin/messages.hpp"

namespace itchbook::soupbin {

// Same shape as emit::Sink, deliberately not shared with it: each
// wire-format module states its own protocol's output contract rather than
// being a client of another module's helper, the precedent itch/ouch/emit
// already set for be16/32/64.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void on_message(const uint8_t* payload, size_t len) = 0;
};

enum class State : uint8_t {
    AwaitingLogin,      // ClientSession: sent Login Request, no reply yet.
                       // ServerSession: connected, no Login Request yet.
    LoginReceived,      // ServerSession only: a well-framed Login Request has
                       // arrived; SoupBinTCP does not decide accept/reject —
                       // that is an authentication POLICY question, out of
                       // this protocol's own stated scope ("SoupBinTCP...
                       // includes a simple scheme that allows the server to
                       // AUTHENTICATE the client" describes the FIELDS, not a
                       // decision this class makes) and out of this phase's
                       // scope, since the gateway that would make it is
                       // 12.5's. The caller must call accept_login() or
                       // reject_login() to move on. No timeout applies to
                       // this wait; the spec gives none, and inventing one
                       // would misattribute it the same way a default
                       // client-dead threshold would.
    LoggedIn,           // steady state; heartbeats and data flow.
    Rejected,           // ClientSession only. reject_reason() is valid.
    Ended,              // received/sent End of Session — a graceful close.
    LoginTimedOut,      // AwaitingLogin's own timeout elapsed unresolved.
                       // Distinct from Dead on purpose: "never got in" and
                       // "was alive and went dark" are different facts a
                       // caller needs to tell apart, not the same failure
                       // reached by two paths through one generic timer.
    Dead,               // LoggedIn's silence timeout elapsed. This is the
                       // state a future 12.5 kill switch watches for.
    ProtocolViolation,  // malformed framing, OR a well-formed packet that
                       // makes no sense in the current state — see the file
                       // banner. Deliberately NOT folded into Dead:
                       // conflating "the peer went silent" with "the peer
                       // (or our own bookkeeping) said something impossible"
                       // would defeat the one thing an operator debugging
                       // "why did my session die" needs to know first.
};

inline bool is_terminal(State s) {
    switch (s) {
        case State::Rejected:
        case State::Ended:
        case State::LoginTimedOut:
        case State::Dead:
        case State::ProtocolViolation:
            return true;
        default:
            return false;
    }
}

namespace detail {

// Tracks one direction's silence. Two independent instances per session —
// one for what THIS side has sent, one for what THIS side has received —
// because the send-side clock answers "do I owe a heartbeat" and the
// receive-side clock answers "has my peer gone quiet," and those are
// different questions with different thresholds.
class SilenceTracker {
public:
    void reset(uint64_t now_ns) { last_ns_ = now_ns; }

    // Monotonic-safe by construction: a backward or repeated now_ns yields
    // ZERO elapsed time, never a spurious trigger and never a wraparound.
    // Adversarial review found this exact gap before any code existed —
    // plain unsigned subtraction on a backward clock read wraps to ~584
    // years and fires every timeout on the very next tick; a naive signed
    // guard fixes that but then silently suppresses detection forever once
    // triggered. Neither failure is hypothetical for a caller feeding
    // timestamps from more than one clock source, or a test harness that
    // replays ticks out of order.
    uint64_t elapsed(uint64_t now_ns) const {
        return now_ns > last_ns_ ? now_ns - last_ns_ : 0;
    }

private:
    uint64_t last_ns_ = 0;
};

// The only completeness test either session class uses, ever: are there at
// least frame_length() bytes buffered? Never "is the header present" alone
// (a partial payload would then be parsed as if complete, synthesizing a
// message from stale bytes past the real end of what arrived), and never a
// derived "bytes still needed" subtraction (buf.size() < need is safe under
// every ordering; need = frame_length - buf.size() underflows to a
// near-SIZE_MAX value the instant it is evaluated on the wrong side of a
// check, and the resulting reserve/resize call is a more realistic
// unbounded-allocation vector than anything a malicious length field alone
// produces).
//
// Drains in a LOOP, not a single dispatch per call: a peer's last few
// packets plus its terminal End of Session arriving coalesced into one final
// TCP read, immediately followed by the socket closing, is not an
// adversarial case — it is what an ordinary graceful shutdown looks like,
// and a single-dispatch-per-call design never sees the terminal packet
// because no further on_bytes() call is coming to drain the rest.
//
// dispatch(type, frame_ptr, frame_len) is called with a pointer to the
// FRAME START, not the payload — matching every other wire-format module in
// this codebase, where an accessor always takes the whole message pointer
// (dispatch.hpp's apply(), split.hpp's aggress()) rather than a
// payload-relative one, so callers can use messages.hpp's own accessors
// directly instead of re-deriving offsets a second time.
class FrameBuffer {
public:
    // A caller passing (nullptr, 0) — a defensive recv()==0 check, or a mock
    // transport treating a no-op call as its own heartbeat — must not be
    // read as "bytes arrived." Handled by the caller checking len before
    // touching any receive-side SilenceTracker; this class itself is simply
    // a no-op appender for zero bytes.
    void append(const uint8_t* data, size_t len) {
        if (len == 0 || data == nullptr) return;
        buf_.insert(buf_.end(), data, data + len);
    }

    // Returns false (and leaves the offending bytes in place, dispatching
    // nothing further this call) the instant a frame is structurally
    // inconsistent per is_valid — an unrecognized type for this role, a
    // fixed-size type whose declared length disagrees with its known size,
    // or a declared length that would make frame_length() exceed
    // max_frame_bytes. The caller is expected to move to ProtocolViolation
    // on a false return. Compacts ONCE per call — a single erase of
    // everything consumed, not one erase per frame — which is what keeps
    // byte-at-a-time delivery of a message linear rather than quadratic in
    // the number of bytes eventually assembled.
    template <typename Dispatch, typename Validate>
    bool drain(size_t max_frame_bytes, Dispatch&& dispatch, Validate&& is_valid) {
        size_t consumed = 0;
        bool ok = true;
        while (ok) {
            const size_t remaining = buf_.size() - consumed;
            if (remaining < 3) break;   // header (length+type) not fully here yet
            const uint8_t* p = buf_.data() + consumed;
            const size_t flen = frame_length(p);
            if (flen > max_frame_bytes) { ok = false; break; }
            if (remaining < flen) break;   // header seen, payload still incomplete
            if (!is_valid(packet_type(p), flen)) { ok = false; break; }
            if (!dispatch(packet_type(p), p, flen)) { ok = false; break; }
            consumed += flen;
        }
        if (consumed > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(consumed));
        return ok;
    }

private:
    std::vector<uint8_t> buf_;
};

// Fixed-size packet types and their known lengths, for validating a decoded
// header against what its own type promises before trusting the payload.
// 0 marks a variable-length type (Sequenced/Unsequenced Data, Debug), which
// this check does not constrain beyond max_frame_bytes.
inline size_t known_fixed_wire_bytes(char type) {
    switch (type) {
        case 'A': return kLoginAcceptedWireBytes;
        case 'J': return kLoginRejectedWireBytes;
        case 'H': return kServerHeartbeatWireBytes;
        case 'Z': return kEndOfSessionWireBytes;
        case 'L': return kLoginRequestWireBytes;
        case 'R': return kClientHeartbeatWireBytes;
        case 'O': return kLogoutRequestWireBytes;
        default:  return 0;
    }
}

}  // namespace detail

// ================================================================================
// ClientSession — the strategy side's view of a SoupBinTCP connection.
// ================================================================================
class ClientSession {
public:
    struct Config {
        uint64_t heartbeat_send_threshold_ns = 1'000'000'000;   // 1s, spec MUST
        uint64_t server_dead_threshold_ns;      // NO DEFAULT — see file banner
        uint64_t login_response_timeout_ns;     // NO DEFAULT — see file banner
        size_t max_frame_bytes = 4096;          // engineering choice, see banner
    };

    // Sends the Login Request immediately, matching section 2.3.1's "must
    // send... immediately upon establishing a new TCP/IP socket connection."
    // `app_in` receives the payload of every Sequenced Data packet — the
    // higher-level (OUCH) message the whole session exists to carry — with
    // the 3-byte SoupBinTCP header already stripped off. Nullable: a caller
    // that only cares about connection health (rare, but this class does
    // not assume otherwise) may pass nullptr and messages are simply
    // dropped after being counted.
    ClientSession(Config cfg, uint64_t start_now_ns, const char* username,
                 const char* password, const char* requested_session,
                 const char* requested_sequence_number, Sink* out, Sink* app_in)
        : cfg_(cfg), out_(out), app_in_(app_in) {
        send_side_.reset(start_now_ns);
        recv_side_.reset(start_now_ns);
        uint8_t frame[soupbin::kLoginRequestWireBytes];
        const size_t n = encode::login_request(frame, username, password,
                                               requested_session,
                                               requested_sequence_number);
        out_->on_message(frame, n);
    }

    // Returns true iff state() changed as a result of this call. A
    // reentrant call — a dispatch reachable from inside this call invoking
    // on_bytes() again — is a caller bug this project treats the way
    // order_types.hpp treats an illegal state transition: aborting in a
    // debug build rather than corrupting the buffer silently. Compiled out
    // under NDEBUG like every assert in this codebase; it is a debug-time
    // safety net; a well-behaved single-threaded event loop never triggers
    // it in the first place, which the tests exercise.
    // `now_ns` is required, not optional, and not cached from a prior
    // tick(): receiving bytes is itself a timestamped event, and a design
    // that inferred "now" from whatever tick() last ran — the first draft of
    // this file did exactly that — resets the receive-idle clock to a stale
    // or, before the first tick(), a zero timestamp. The bug that caught
    // this: a session ticked with a deliberately-backward clock (to prove
    // the SilenceTracker guard above) went Dead on the very next tick
    // regardless, because on_bytes() had anchored recv_side_ at time zero
    // rather than at the time the bytes actually arrived.
    bool on_bytes(const uint8_t* data, size_t len, uint64_t now_ns) {
        assert(!in_dispatch_ && "reentrant ClientSession::on_bytes()");
        if (len == 0 || data == nullptr) return false;   // not activity
        const State before = state_;
        in_dispatch_ = true;
        buf_.append(data, len);
        const bool ok = buf_.drain(
            cfg_.max_frame_bytes,
            [&](char type, const uint8_t* frame, size_t flen) {
                return dispatch(type, frame, flen);
            },
            [](char type, size_t flen) {
                const size_t known = detail::known_fixed_wire_bytes(type);
                if (type != 'A' && type != 'J' && type != 'H' && type != 'Z' &&
                    type != 'S' && type != '+') {
                    return false;   // not a type a client ever legitimately receives
                }
                return known == 0 || flen == known;
            });
        in_dispatch_ = false;
        if (!ok) state_ = State::ProtocolViolation;
        else if (!is_terminal(before) && !is_terminal(state_)) recv_side_.reset(now_ns);
        return state_ != before;
    }

    // MUST be called on an independent periodic timer — see the file banner.
    bool tick(uint64_t now_ns) {
        if (is_terminal(state_)) return false;
        const State before = state_;

        if (state_ == State::AwaitingLogin) {
            if (recv_side_.elapsed(now_ns) >= cfg_.login_response_timeout_ns) {
                state_ = State::LoginTimedOut;
            }
        } else if (state_ == State::LoggedIn) {
            if (recv_side_.elapsed(now_ns) >= cfg_.server_dead_threshold_ns) {
                state_ = State::Dead;
            }
        }

        // Heartbeat-send obligation applies from construction, unconditional
        // per the literal spec text (see file banner) — checked after the
        // state transitions above so a session that just went terminal this
        // tick does not also emit a trailing heartbeat.
        if (!is_terminal(state_) &&
            send_side_.elapsed(now_ns) >= cfg_.heartbeat_send_threshold_ns) {
            uint8_t frame[soupbin::kClientHeartbeatWireBytes];
            const size_t n = encode::client_heartbeat(frame);
            out_->on_message(frame, n);
            send_side_.reset(now_ns);
        }
        return state_ != before;
    }

    void send_unsequenced(const uint8_t* msg, size_t len, uint64_t now_ns) {
        std::vector<uint8_t> frame(len + 3);
        encode::unsequenced_data(frame.data(), msg, len);
        out_->on_message(frame.data(), frame.size());
        send_side_.reset(now_ns);
    }

    void send_logout(uint64_t now_ns) {
        uint8_t frame[soupbin::kLogoutRequestWireBytes];
        const size_t n = encode::logout_request(frame);
        out_->on_message(frame, n);
        send_side_.reset(now_ns);
    }

    State state() const { return state_; }
    char reject_reason() const { return reject_reason_; }         // valid iff Rejected
    const uint8_t* session_id() const { return session_id_; }      // valid iff LoggedIn+
    // Local count, per section 2.2.3: sequence numbers are never on the
    // wire; both sides count Sequenced Data Packets as they arrive/send.
    uint64_t received_sequence_number() const { return sequence_number_; }

private:
    bool dispatch(char type, const uint8_t* p, size_t /*flen*/) {
        switch (type) {
            case '+':
                return true;   // Debug: ignored by design, per spec 2.1
            case 'A':
                if (state_ != State::AwaitingLogin) return false;   // out-of-state
                std::memcpy(session_id_, login_accepted::session(p), 10);
                state_ = State::LoggedIn;
                return true;
            case 'J':
                if (state_ != State::AwaitingLogin) return false;
                reject_reason_ = login_rejected::reason(p);
                state_ = State::Rejected;
                return true;
            case 'H':
                return true;   // the SilenceTracker reset already covers it
            case 'S':
                if (state_ != State::LoggedIn) return false;   // 2.2.1's ordering promise
                ++sequence_number_;
                if (app_in_ != nullptr) app_in_->on_message(sequenced_data::message(p),
                                                            sequenced_data::message_length(p));
                return true;
            case 'Z':
                if (state_ != State::LoggedIn) return false;
                state_ = State::Ended;
                return true;
            default:
                return false;   // unreachable: is_valid already rejected anything else
        }
    }

    Config cfg_;
    Sink* out_;
    Sink* app_in_;
    detail::FrameBuffer buf_;
    detail::SilenceTracker send_side_;
    detail::SilenceTracker recv_side_;
    State state_ = State::AwaitingLogin;
    char reject_reason_ = 0;
    uint8_t session_id_[10] = {};
    uint64_t sequence_number_ = 0;
    bool in_dispatch_ = false;
};

// ================================================================================
// ServerSession — the exchange gateway's view of one client connection.
// ================================================================================
class ServerSession {
public:
    struct Config {
        uint64_t heartbeat_send_threshold_ns = 1'000'000'000;   // 1s, spec MUST
        uint64_t client_dead_threshold_ns = 15'000'000'000;     // 15s, "typically"
        uint64_t login_timeout_ns = 30'000'000'000;             // 30s, "typically"
        size_t max_frame_bytes = 4096;
    };

    // `app_in` receives the payload of every Unsequenced Data packet — the
    // client's higher-level (OUCH) messages, header stripped — once the
    // session is LoggedIn. Nullable, same as ClientSession's.
    ServerSession(Config cfg, uint64_t start_now_ns, Sink* out, Sink* app_in)
        : cfg_(cfg), out_(out), app_in_(app_in) {
        send_side_.reset(start_now_ns);
        recv_side_.reset(start_now_ns);
    }

    // `now_ns` required, not cached — see ClientSession::on_bytes()'s
    // comment; the same staleness bug applies symmetrically here.
    bool on_bytes(const uint8_t* data, size_t len, uint64_t now_ns) {
        assert(!in_dispatch_ && "reentrant ServerSession::on_bytes()");
        if (len == 0 || data == nullptr) return false;
        const State before = state_;
        in_dispatch_ = true;
        buf_.append(data, len);
        const bool ok = buf_.drain(
            cfg_.max_frame_bytes,
            [&](char type, const uint8_t* frame, size_t flen) {
                return dispatch(type, frame, flen);
            },
            [](char type, size_t flen) {
                const size_t known = detail::known_fixed_wire_bytes(type);
                if (type != 'L' && type != 'U' && type != 'R' && type != 'O' &&
                    type != '+') {
                    return false;   // not a type a server ever legitimately receives
                }
                return known == 0 || flen == known;
            });
        in_dispatch_ = false;
        if (!ok) state_ = State::ProtocolViolation;
        else if (!is_terminal(before) && !is_terminal(state_)) recv_side_.reset(now_ns);
        return state_ != before;
    }

    // MUST be called on an independent periodic timer — see the file banner.
    bool tick(uint64_t now_ns) {
        if (is_terminal(state_)) return false;
        const State before = state_;

        if (state_ == State::AwaitingLogin) {
            if (recv_side_.elapsed(now_ns) >= cfg_.login_timeout_ns) {
                state_ = State::LoginTimedOut;
            }
            // LoginReceived has NO timeout of its own — see the State enum's
            // own comment. Nothing to check here for it.
        } else if (state_ == State::LoggedIn) {
            if (recv_side_.elapsed(now_ns) >= cfg_.client_dead_threshold_ns) {
                state_ = State::Dead;
            }
        }

        // Gated to LoggedIn only: sending a heartbeat before Login
        // Accepted/Rejected would make it, not the login response, the
        // first non-debug packet — see the file banner's asymmetry note.
        if (state_ == State::LoggedIn &&
            send_side_.elapsed(now_ns) >= cfg_.heartbeat_send_threshold_ns) {
            uint8_t frame[soupbin::kServerHeartbeatWireBytes];
            const size_t n = encode::server_heartbeat(frame);
            out_->on_message(frame, n);
            send_side_.reset(now_ns);
        }
        return state_ != before;
    }

    // The application layer's decision, not this class's — see LoginReceived.
    void accept_login(const char* session_id, const char* sequence_number, uint64_t now_ns) {
        assert(state_ == State::LoginReceived);
        uint8_t frame[soupbin::kLoginAcceptedWireBytes];
        const size_t n = encode::login_accepted(frame, session_id, sequence_number);
        out_->on_message(frame, n);
        send_side_.reset(now_ns);   // this IS the first non-debug send
        state_ = State::LoggedIn;
    }

    void reject_login(char reason) {
        assert(state_ == State::LoginReceived);
        uint8_t frame[soupbin::kLoginRejectedWireBytes];
        const size_t n = encode::login_rejected(frame, reason);
        out_->on_message(frame, n);
        // Section 2.2.2: the server closes the socket after this. This
        // class does not own a socket to close; Rejected is the signal a
        // real transport adapter closes on.
        state_ = State::Rejected;
    }

    void send_sequenced(const uint8_t* msg, size_t len, uint64_t now_ns) {
        std::vector<uint8_t> frame(len + 3);
        encode::sequenced_data(frame.data(), msg, len);
        out_->on_message(frame.data(), frame.size());
        send_side_.reset(now_ns);
        ++sequence_number_;
    }

    void send_end_of_session() {
        uint8_t frame[soupbin::kEndOfSessionWireBytes];
        const size_t n = encode::end_of_session(frame);
        out_->on_message(frame, n);
        state_ = State::Ended;
    }

    State state() const { return state_; }
    uint64_t sequence_number() const { return sequence_number_; }
    // Valid once state() has reached LoginReceived or later; reads directly
    // off the stored 49-byte Login Request frame via messages.hpp's own
    // accessors, rather than re-deriving the offsets a second time.
    const uint8_t* login_username() const { return login_request::username(login_frame_); }
    const uint8_t* login_password() const { return login_request::password(login_frame_); }
    const uint8_t* login_requested_session() const {
        return login_request::requested_session(login_frame_);
    }
    const uint8_t* login_requested_sequence_number() const {
        return login_request::requested_sequence_number(login_frame_);
    }

private:
    bool dispatch(char type, const uint8_t* p, size_t /*flen*/) {
        switch (type) {
            case '+':
                return true;
            case 'L':
                if (state_ != State::AwaitingLogin) return false;   // out-of-state
                std::memcpy(login_frame_, p, soupbin::kLoginRequestWireBytes);
                state_ = State::LoginReceived;
                return true;
            case 'U':
                if (state_ != State::LoggedIn) return false;   // spec 2.3.2: only
                                                                // "at any time AFTER the
                                                                // Login Accepted Packet"
                if (app_in_ != nullptr) app_in_->on_message(unsequenced_data::message(p),
                                                            unsequenced_data::message_length(p));
                return true;
            case 'R':
                return true;   // the SilenceTracker reset already covers it
            case 'O':
                if (state_ != State::LoggedIn && state_ != State::LoginReceived) return false;
                state_ = State::Ended;
                return true;
            default:
                return false;
        }
    }

    Config cfg_;
    Sink* out_;
    Sink* app_in_;
    detail::FrameBuffer buf_;
    detail::SilenceTracker send_side_;
    detail::SilenceTracker recv_side_;
    State state_ = State::AwaitingLogin;
    uint64_t sequence_number_ = 1;   // "the sequence number of the first
                                    // sequenced message... is always 1" (2.2.3)
    uint8_t login_frame_[soupbin::kLoginRequestWireBytes] = {};
    bool in_dispatch_ = false;
};

}  // namespace itchbook::soupbin
