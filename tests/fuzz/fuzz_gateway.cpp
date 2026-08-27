// fuzz_gateway — phase 12.6's done-condition.
//
// Random OUCH order entry, interleaved with historical ITCH traffic against
// the SAME shared book, with the invariants checked after every operation.
// The interleaving is what makes this a gateway fuzz rather than a second
// matcher fuzz: the two fill paths it exists to differentiate are
//
//   * the strategy crosses  -- Matcher::match() against a maker it does not
//     own, which before 12.6 did not fill at all and left the book crossed;
//   * the strategy is hit   -- Matcher::apply_external_fill(), driven by the
//     phase-12.1 aggressor, which before 12.6 reduced a strategy order behind
//     the Matcher's back while conserves_shares() reported true.
//
// Both were proven with running programs before this file was written, and
// each is counted below: a run that reaches neither has not exercised the
// thing 12.6 is about, and the coverage report says so out loud rather than
// reporting a green pass over an untested path.
//
// Built WITH sanitizers, unlike fuzz_matcher. That opt-out was a throughput
// decision for a 200,000-sequence ctest run; the cost here was measured
// rather than inherited -- roughly 110k ops/s under ASan/UBSan, so the
// done-condition's million operations is seconds, not minutes.
//
//   g++ -O2 -Iinclude -I. tests/fuzz/fuzz_gateway.cpp -o fuzz_gateway
//   ./fuzz_gateway --iterations 50000
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/emit/sink.hpp"
#include "itchbook/engine/gateway.hpp"
#include "itchbook/ouch/encode.hpp"
#include "itchbook/ouch/messages.hpp"
#include "itchbook/replay/split.hpp"
#include "itchbook/soupbin/session.hpp"

namespace {

namespace eng = itchbook::engine;
namespace ouch = itchbook::ouch;
namespace sb = itchbook::soupbin;
namespace bk = itchbook::book;
namespace rp = itchbook::replay;

constexpr uint16_t LOCATE = 7;
constexpr int32_t BASE = 1000000;      // $100.0000
constexpr int32_t TICK = 100;
constexpr uint64_t SEC = 1'000'000'000ULL;

// ---- coverage: what the generator actually reached ---------------------------
//
// fuzz_matcher carries one of these because for a long time the answer for
// stop orders was zero -- the type switch never selected them and a million
// sequences exercised four of six types. The same trap applies here with more
// force, because the two paths this file exists to test are the two hardest
// to reach by accident.
struct Coverage {
    uint64_t ops = 0;
    uint64_t accepted = 0, rejected = 0, cancelled = 0, replaced = 0;
    uint64_t malformed = 0, dup_token = 0, unknown_token = 0;
    uint64_t strategy_crossed = 0;      // our order took historical liquidity
    uint64_t strategy_hit = 0;          // the aggressor took ours
    uint64_t historical_adds = 0, historical_execs = 0;
    uint64_t kill_rejects = 0, flattens = 0;
    uint64_t ouch_executed = 0, itch_emitted = 0;

    void report() const {
        std::printf("  operations                 %llu\n", (unsigned long long)ops);
        std::printf("  accepted / rejected        %llu / %llu\n",
                    (unsigned long long)accepted, (unsigned long long)rejected);
        std::printf("  cancelled / replaced       %llu / %llu\n",
                    (unsigned long long)cancelled, (unsigned long long)replaced);
        std::printf("  malformed / dup / unknown  %llu / %llu / %llu\n",
                    (unsigned long long)malformed, (unsigned long long)dup_token,
                    (unsigned long long)unknown_token);
        std::printf("  historical adds / execs    %llu / %llu\n",
                    (unsigned long long)historical_adds,
                    (unsigned long long)historical_execs);
        std::printf("  STRATEGY CROSSED           %llu\n",
                    (unsigned long long)strategy_crossed);
        std::printf("  STRATEGY HIT BY AGGRESSOR  %llu\n",
                    (unsigned long long)strategy_hit);
        std::printf("  kill rejects / flattens    %llu / %llu\n",
                    (unsigned long long)kill_rejects, (unsigned long long)flattens);
        std::printf("  OUCH Executed / ITCH sent  %llu / %llu\n",
                    (unsigned long long)ouch_executed, (unsigned long long)itch_emitted);
    }
};

// ---- sinks --------------------------------------------------------------------

class WireSink : public sb::Sink {
public:
    std::vector<std::vector<uint8_t>> msgs;
    void on_message(const uint8_t* p, size_t n) override { msgs.emplace_back(p, p + n); }
    void clear() { msgs.clear(); }
};

// The published ITCH, consumed straight back into an independent book -- the
// "book built by replaying the emitted ITCH equals the matcher's book" half of
// the differential.
class ItchSink : public itchbook::emit::Sink {
public:
    bk::BookSet consumer{1u << 14, 100, 20, 64};
    uint64_t count = 0;
    void on_message(const uint8_t* p, size_t n) override {
        ++count;
        (void)n;
        bk::apply(consumer, static_cast<char>(p[0]), p);
    }
};

struct Bytes {
    const uint8_t* p;
    size_t n, i = 0;
    uint8_t u8() { return i < n ? p[i++] : 0; }
    uint32_t u32() { return uint32_t(u8()) | (uint32_t(u8()) << 8); }
    uint32_t range(uint32_t lo, uint32_t hi) {
        return hi <= lo ? lo : lo + (u32() % (hi - lo + 1));
    }
    bool done() const { return i >= n; }
};

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void put64(uint8_t* p, uint64_t v) {
    for (int k = 0; k < 8; ++k) p[k] = uint8_t(v >> (56 - 8 * k));
}

// ---- one sequence ---------------------------------------------------------------

bool run_sequence(const uint8_t* data, size_t len, Coverage& cov, std::string* why) {
    Bytes in{data, len};

    bk::BookSet set{1u << 14, 100, 20, 64};
    bk::Book& book = set.at(LOCATE);
    eng::Matcher matcher{book, TICK};
    eng::RefSource refs;
    itchbook::risk::KillSwitchConfig kcfg;
    kcfg.max_position = 100000;
    itchbook::risk::KillSwitch kill{kcfg};

    WireSink wire;
    ItchSink itch;
    sb::ServerSession session{{}, 0, &wire, nullptr};

    eng::Gateway::Config gcfg;
    std::memcpy(gcfg.stock, "TEST    ", 8);
    gcfg.collar_ticks = 0;          // collar off: it is unit-tested, and on
                                    // here it would reject most of the traffic
    gcfg.max_position = 0;
    eng::Gateway gw{gcfg, session, matcher, refs, kill};
    gw.set_itch_sink(&itch);

    rp::SplitReplayer replayer{set};
    replayer.set_matcher(&matcher);
    // ONE published feed. A subscriber sees the union of what the replayer
    // publishes for historical mutations (12.2) and what the gateway
    // publishes for the engine's, so both go to the same sink.
    replayer.set_sink(&itch);

    // Log in.
    uint8_t lr[sb::kLoginRequestWireBytes];
    sb::encode::login_request(lr, "U", "P", "", "1");
    session.on_bytes(lr, sizeof(lr), 0);
    gw.decide_login(true, "SESS1", "1", 0);
    wire.clear();

    // The harness's own ledger, built ONLY from what it sent and what came
    // back on the wire -- never by asking the Gateway or the Matcher.
    struct Live { bool live = false; uint64_t ref = 0; };
    std::unordered_map<std::string, Live> ledger;
    std::vector<std::string> tokens;
    std::vector<uint64_t> hist_refs;
    uint64_t hist_next = 1;
    uint64_t now = SEC;
    uint64_t prev_strategy_hit = 0;

    auto tok_of = [&](uint32_t k) {
        char b[15];
        std::snprintf(b, sizeof(b), "T%012u", k);
        return std::string(b);
    };

    for (int step = 0; step < 40 && !in.done(); ++step) {
        now += SEC / 4;
        const uint8_t op = in.u8() % 14;
        ++cov.ops;
        const size_t fills_before = matcher.fills().size();
        const uint64_t hit_before = replayer.counters().strategy_shares_taken;

        switch (op) {
            case 0: case 1: case 2: {          // Enter, limit
                const std::string t = tok_of(in.range(0, 40));
                const char side = (in.u8() & 1) ? 'B' : 'S';
                const uint32_t sh = in.range(1, 5) * 100;
                const int32_t px = BASE + int32_t(in.range(0, 20)) * TICK - 10 * TICK;
                std::vector<uint8_t> m(ouch::kEnterOrderLen);
                ouch::encode::enter_order(m.data(), t.c_str(), side, sh, "TEST    ",
                                          px, 0, "FIRM", 'Y', 'A', 'N', 0, 'N', ' ');
                if (ledger.count(t) != 0) ++cov.dup_token;
                gw.on_ouch(m.data(), m.size(), now);
                break;
            }
            case 3: {                          // Enter, market
                const std::string t = tok_of(in.range(0, 40));
                const char side = (in.u8() & 1) ? 'B' : 'S';
                const int32_t px = (in.u8() & 1) ? ouch::kMarketOrderThreshold
                                                 : ouch::kMarketOrderPrice;
                std::vector<uint8_t> m(ouch::kEnterOrderLen);
                ouch::encode::enter_order(m.data(), t.c_str(), side, in.range(1, 3) * 100,
                                          "TEST    ", px, 0, "FIRM", 'Y', 'A', 'N', 0,
                                          'N', ' ');
                gw.on_ouch(m.data(), m.size(), now);
                break;
            }
            case 4: {                          // Replace
                if (tokens.empty()) break;
                const std::string& old_t = tokens[in.range(0, uint32_t(tokens.size() - 1))];
                const std::string fresh = tok_of(in.range(100, 200));
                std::vector<uint8_t> m(ouch::kReplaceOrderLen);
                ouch::encode::replace_order(m.data(), old_t.c_str(), fresh.c_str(),
                                            in.range(1, 4) * 100,
                                            BASE + int32_t(in.range(0, 20)) * TICK - 10 * TICK,
                                            0, 'Y', 'N', 0);
                gw.on_ouch(m.data(), m.size(), now);
                break;
            }
            case 5: {                          // Cancel
                if (tokens.empty()) break;
                const std::string& t = tokens[in.range(0, uint32_t(tokens.size() - 1))];
                std::vector<uint8_t> m(ouch::kCancelOrderLen);
                ouch::encode::cancel_order(m.data(), t.c_str(), 0);
                gw.on_ouch(m.data(), m.size(), now);
                break;
            }
            case 6: {                          // Cancel, token never seen
                std::vector<uint8_t> m(ouch::kCancelOrderLen);
                ouch::encode::cancel_order(m.data(), "NEVERSEEN0001", 0);
                ++cov.unknown_token;
                gw.on_ouch(m.data(), m.size(), now);
                break;
            }
            case 7: {                          // malformed
                ++cov.malformed;
                const uint8_t kind = in.u8() % 4;
                std::vector<uint8_t> m;
                if (kind == 0) { /* empty */ }
                else if (kind == 1) { m.assign(ouch::kEnterOrderLen, 0); m[0] = '?'; }
                else if (kind == 2) { m.assign(ouch::kEnterOrderLen - 1, 0); m[0] = 'O'; }
                else { m.assign(ouch::kEnterOrderLen, 0); m[0] = 'X'; }
                const bool ok = gw.on_ouch(m.empty() ? nullptr : m.data(), m.size(), now);
                if (ok) { *why = "malformed OUCH accepted"; return false; }
                break;
            }
            case 8: case 9: {                  // historical add, same price band
                const uint64_t ref = ++hist_next;   // bit 63 clear
                const char side = (in.u8() & 1) ? 'B' : 'S';
                const int32_t px = BASE + int32_t(in.range(0, 20)) * TICK - 10 * TICK;
                std::vector<uint8_t> m(36, 0);
                m[0] = 'A';
                put16(m.data() + 1, LOCATE);
                put64(m.data() + 11, ref);
                m[19] = uint8_t(side);
                put32(m.data() + 20, in.range(1, 5) * 100);
                std::memcpy(m.data() + 24, "TEST    ", 8);
                put32(m.data() + 32, uint32_t(px));
                replayer.apply('A', m.data());
                hist_refs.push_back(ref);
                ++cov.historical_adds;
                break;
            }
            case 10: case 11: {                // historical execution -> aggressor
                if (hist_refs.empty()) break;
                const uint64_t ref = hist_refs[in.range(0, uint32_t(hist_refs.size() - 1))];
                std::vector<uint8_t> m(31, 0);
                m[0] = 'E';
                put16(m.data() + 1, LOCATE);
                put64(m.data() + 11, ref);
                put32(m.data() + 19, in.range(1, 5) * 100);
                replayer.apply('E', m.data());
                matcher.pump_stops();
                ++cov.historical_execs;
                break;
            }
            case 12: {                         // historical delete
                if (hist_refs.empty()) break;
                const uint64_t ref = hist_refs[in.range(0, uint32_t(hist_refs.size() - 1))];
                std::vector<uint8_t> m(19, 0);
                m[0] = 'D';
                put16(m.data() + 1, LOCATE);
                put64(m.data() + 11, ref);
                replayer.apply('D', m.data());
                break;
            }
            default: {                         // heartbeat / time passes
                session.tick(now);
                break;
            }
        }

        // The aggressor can fill our resting orders with no call of ours on
        // the stack; the cursor is what publishes those.
        gw.pump_fills(now);
        gw.poll(now);

        // ---- classify what happened, from the wire only ----------------------
        for (const auto& msg : wire.msgs) {
            if (msg.size() <= 3) continue;
            const uint8_t* o = msg.data() + 3;
            const char t = static_cast<char>(o[0]);
            char raw[15];
            if (t == 'A') {
                std::memcpy(raw, ouch::accepted::order_token(o), 14);
                raw[14] = 0;
                std::string tk(raw);
                while (!tk.empty() && tk.back() == ' ') tk.pop_back();
                ledger[tk] = Live{true, ouch::accepted::reference_number(o)};
                tokens.push_back(tk);
                ++cov.accepted;
                if (!rp::is_strategy_ref(ouch::accepted::reference_number(o))) {
                    *why = "Accepted carried a reference outside the strategy half";
                    return false;
                }
            } else if (t == 'J') {
                ++cov.rejected;
                if (ouch::rejected::reason(o) == ouch::reject_reason::kHalted) {
                    ++cov.kill_rejects;
                }
            } else if (t == 'C') {
                ++cov.cancelled;
            } else if (t == 'U') {
                ++cov.replaced;
            } else if (t == 'E') {
                ++cov.ouch_executed;
            }
        }
        wire.clear();

        // ---- the two paths 12.6 exists to differentiate ----------------------
        for (size_t k = fills_before; k < matcher.fills().size(); ++k) {
            const eng::Fill& f = matcher.fills()[k];
            if (f.external && f.taker != 0) ++cov.strategy_crossed;
        }
        if (replayer.counters().strategy_shares_taken != hit_before) ++cov.strategy_hit;
        prev_strategy_hit = replayer.counters().strategy_shares_taken;
        (void)prev_strategy_hit;

        // ---- invariants, after EVERY operation -------------------------------
        if (!matcher.conserves_shares()) { *why = "shares not conserved"; return false; }
        if (!matcher.agrees_with_book()) { *why = "Meta disagrees with the book"; return false; }
        if (replayer.counters().partition_violations != 0) {
            *why = "reference partition violated"; return false;
        }
        // Every Meta is in exactly one consistent shape.
        for (const auto& [id, m] : matcher.orders()) {
            (void)id;
            if (eng::is_terminal(m.state) && (m.in_book || m.resting != 0)) {
                *why = "terminal order still resting"; return false;
            }
            if (m.in_book && m.resting == 0) {
                *why = "in_book with nothing resting"; return false;
            }
        }
        // Every strategy reference resting in the book is one the engine knows.
        for (char sd : {'B', 'S'}) {
            std::vector<bk::LevelView> lv;
            book.top(sd, 8, &lv);
            for (const auto& l : lv) {
                for (const bk::Order* o = book.first_order(sd, l.price); o != nullptr;
                     o = o->next) {
                    if (!rp::is_strategy_ref(o->ref)) continue;
                    const eng::Matcher::Meta* mm = matcher.find(o->ref);
                    if (mm == nullptr || !mm->in_book || mm->resting != o->shares) {
                        *why = "a strategy order in the book the engine has lost";
                        return false;
                    }
                }
            }
        }
    }

    cov.itch_emitted += itch.count;

    // ---- the cross-protocol differential, at the end of the sequence --------
    //
    // The book a consumer rebuilds from the PUBLISHED ITCH must hold every
    // strategy order the exchange's own book holds, at the same size. Only
    // strategy references are compared: the consumer never saw the historical
    // orders, which arrive on the real feed rather than from this gateway.
    const bk::Book* cb = itch.consumer.peek(LOCATE);
    if (cb == nullptr) {
        // Only legitimate if nothing was ever published.
        if (itch.count != 0) { *why = "feed published but no consumer book"; return false; }
        return true;
    }
    // Every order the exchange holds, the subscriber holds, at the same size,
    // price and side -- historical and strategy alike, since the subscriber
    // now sees the whole feed.
    for (char sd : {'B', 'S'}) {
        std::vector<bk::LevelView> lv;
        book.top(sd, 32, &lv);
        for (const auto& l : lv) {
            for (const bk::Order* o = book.first_order(sd, l.price); o != nullptr;
                 o = o->next) {
                const bk::Order* co = cb->find(o->ref);
                if (co == nullptr) {
                    *why = "published feed is missing an order the book holds";
                    return false;
                }
                if (co->shares != o->shares || co->price != o->price ||
                    co->side != o->side) {
                    *why = "published feed disagrees with the book about an order";
                    return false;
                }
            }
        }
    }
    // And the converse: the subscriber holds nothing the exchange does not.
    // Without this the check passes for a feed that invents orders.
    if (cb->resting_orders() != book.resting_orders() ||
        cb->resting_shares() != book.resting_shares()) {
        *why = "subscriber holds a different amount than the exchange";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t iterations = 20000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    std::mt19937_64 rng(12345);
    Coverage cov;
    std::vector<uint8_t> buf(256);
    std::string why;

    for (uint64_t it = 0; it < iterations; ++it) {
        for (auto& b : buf) b = uint8_t(rng() & 0xFF);
        if (!run_sequence(buf.data(), buf.size(), cov, &why)) {
            std::fprintf(stderr, "FAIL at iteration %llu: %s\n",
                         (unsigned long long)it, why.c_str());
            return 1;
        }
    }

    std::printf("fuzz_gateway: %llu sequences clean\n", (unsigned long long)iterations);
    cov.report();

    // A run that never reached either fill path has not tested what this file
    // is for, and must say so rather than reporting a green pass.
    int missing = 0;
    if (cov.strategy_crossed == 0) {
        std::fprintf(stderr, "COVERAGE GAP: the strategy never crossed historical liquidity\n");
        ++missing;
    }
    if (cov.strategy_hit == 0) {
        std::fprintf(stderr, "COVERAGE GAP: the aggressor never hit a strategy order\n");
        ++missing;
    }
    if (cov.ouch_executed == 0) {
        std::fprintf(stderr, "COVERAGE GAP: no OUCH Executed was ever sent\n");
        ++missing;
    }
    if (cov.malformed == 0 || cov.accepted == 0 || cov.rejected == 0) {
        std::fprintf(stderr, "COVERAGE GAP: a basic outcome class was never reached\n");
        ++missing;
    }
    return missing == 0 ? 0 : 1;
}
