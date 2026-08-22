// The closed-loop driver, and the invariant that keeps it honest.
//
// closed_loop.hpp orchestrates the same components as backtest.hpp in the same
// order, and that similarity is the risk: two copies of one piece of logic
// either disagree on a trailing zero or agree while both being wrong. The 10.6
// determinism gate made the same argument about two book writers and the answer
// was the same -- do not trust a comment, write the comparison.
//
// The claim: a strategy that IGNORES its fills is answering the same question in
// both drivers, so both must give the same answer, field for field. Any drift in
// ordering -- when actions arrive, whether the kill switch sees a mark before or
// after a fill, how the residual is marked -- breaks it and names the field.
#include <cstdint>
#include <string>
#include <vector>

#include "itchbook/sim/backtest.hpp"
#include "itchbook/sim/closed_loop.hpp"
#include "itchbook/sim/inventory_strategy.hpp"
#include "itchbook/sim/strategies.hpp"
#include "tests/check.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

// ---- a feed with quoting on both sides and enough movement to fill ---------
//
// Built here rather than generated, so the test is about the drivers rather
// than about a generator.
struct Feed {
    std::vector<std::vector<uint8_t>> msgs;
    uint64_t ts = 34200ULL * 1000000000ULL;   // 09:30:00
    uint64_t next_ref = 1;

    std::vector<uint8_t>& add(char type, size_t len) {
        msgs.emplace_back(len, 0);
        auto& m = msgs.back();
        m[0] = static_cast<uint8_t>(type);
        m[1] = 0; m[2] = 1;                    // locate 1
        ts += 1000000;                         // 1 ms apart
        for (int i = 0; i < 6; ++i) {
            m[static_cast<size_t>(5 + i)] =
                static_cast<uint8_t>((ts >> (8 * (5 - i))) & 0xff);
        }
        return m;
    }
    static void be64(std::vector<uint8_t>& m, size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            m[off + static_cast<size_t>(i)] =
                static_cast<uint8_t>((v >> (8 * (7 - i))) & 0xff);
    }
    static void be32(std::vector<uint8_t>& m, size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            m[off + static_cast<size_t>(i)] =
                static_cast<uint8_t>((v >> (8 * (3 - i))) & 0xff);
    }
    void system_event(char code) { add('S', 12)[11] = static_cast<uint8_t>(code); }
    uint64_t add_order(char side, uint32_t shares, int32_t price) {
        const uint64_t ref = next_ref++;
        auto& m = add('A', 36);
        be64(m, 11, ref);
        m[19] = static_cast<uint8_t>(side);
        be32(m, 20, shares);
        for (size_t i = 0; i < 8; ++i) m[24 + i] = static_cast<uint8_t>(' ');
        m[24] = 'T'; m[25] = 'E'; m[26] = 'S'; m[27] = 'T';
        be32(m, 32, static_cast<uint32_t>(price));
        return ref;
    }
    void execute(uint64_t ref, uint32_t shares) {
        auto& m = add('E', 31);
        be64(m, 11, ref);
        be32(m, 19, shares);
        be64(m, 23, next_ref++);
    }
    void delete_order(uint64_t ref) { be64(add('D', 19), 11, ref); }
};

Feed build_feed() {
    Feed f;
    f.system_event('O');
    f.system_event('Q');
    // A two-sided book that walks up and back down, with executions on both
    // sides so a resting quote of ours can actually trade.
    for (int round = 0; round < 40; ++round) {
        const int32_t drift = static_cast<int32_t>((round % 8) - 4) * 100;
        const uint64_t b1 = f.add_order('B', 400, 999000 + drift);
        const uint64_t b2 = f.add_order('B', 300, 998900 + drift);
        const uint64_t a1 = f.add_order('S', 400, 999100 + drift);
        const uint64_t a2 = f.add_order('S', 300, 999200 + drift);
        f.execute(b1, 200);
        f.execute(a1, 200);
        f.delete_order(b2);
        f.delete_order(a2);
        f.delete_order(b1);
        f.delete_order(a1);
    }
    f.system_event('M');
    f.system_event('C');
    return f;
}

// The same maker, written against the inventory interface but ignoring the
// inventory. Deliberately identical in behaviour to TouchMaker: it is the
// control, and if it drifted from TouchMaker the comparison below would be
// comparing two strategies rather than two drivers.
struct BlindMaker {
    uint64_t next_id = 1;
    int32_t bid = 0;
    int32_t ask = 0;
    uint64_t bid_id = 0;
    uint64_t ask_id = 0;

    void quote(const MarketView& v, Ctx& ctx) {
        int32_t b = 0;
        int32_t a = 0;
        if (!v.tradable || !v.best_bid(&b) || !v.best_ask(&a)) return;
        if (b != bid) {
            if (bid_id != 0) ctx.cancel(bid_id);
            bid_id = next_id++;
            ctx.quote(bid_id, Side::Buy, b, 100);
            bid = b;
        }
        if (a != ask) {
            if (ask_id != 0) ctx.cancel(ask_id);
            ask_id = next_id++;
            ctx.quote(ask_id, Side::Sell, a, 100);
            ask = a;
        }
    }
    void on_event(const MarketView& v, Ctx& ctx) { quote(v, ctx); }
};

struct BlindInventoryMaker {
    BlindMaker inner;
    void on_event(const InventoryView& v, Ctx& ctx) { inner.on_event(v, ctx); }
    // No on_fill: this is the whole point. The strategy is told nothing.
};

// Whether a type exposes queue position, as a value the compiler can reason
// about. It goes through a template parameter deliberately: a requires-
// expression written directly on a concrete type is checked eagerly, so
// `requires(FillEvent f) { f.ahead_at_arrival; }` is a hard error about a
// missing member rather than the `false` it is meant to evaluate to. Making the
// type a parameter puts the substitution back in SFINAE territory.
template <typename T>
constexpr bool exposes_queue_position = requires(T f) { f.ahead_at_arrival; };

std::string differences(const LaneResult& a, const LaneResult& b) {
    std::string out;
    auto cmp = [&out](const char* name, long long x, long long y) {
        if (x != y) {
            out += std::string(name) + " " + std::to_string(x) + " vs " +
                   std::to_string(y) + "; ";
        }
    };
    cmp("fills", static_cast<long long>(a.fills), static_cast<long long>(b.fills));
    cmp("shares", static_cast<long long>(a.shares), static_cast<long long>(b.shares));
    cmp("equity", a.equity, b.equity);
    cmp("edge", a.edge, b.edge);
    cmp("drift", a.drift, b.drift);
    cmp("fees", a.fees, b.fees);
    cmp("equity_per_share", a.equity_per_share, b.equity_per_share);
    cmp("residual_position", a.residual_position, b.residual_position);
    cmp("clamp_events", static_cast<long long>(a.clamp_events),
        static_cast<long long>(b.clamp_events));
    cmp("priority_anomalies", static_cast<long long>(a.priority_anomalies),
        static_cast<long long>(b.priority_anomalies));
    cmp("lock_fills", static_cast<long long>(a.lock_fills),
        static_cast<long long>(b.lock_fills));
    cmp("through_fills", static_cast<long long>(a.through_fills),
        static_cast<long long>(b.through_fills));
    cmp("suppressed_quotes", static_cast<long long>(a.suppressed_quotes),
        static_cast<long long>(b.suppressed_quotes));
    cmp("peak_position", a.peak_position, b.peak_position);
    cmp("trip", static_cast<long long>(a.trip), static_cast<long long>(b.trip));
    for (size_t h = 0; h < kNumHorizons; ++h) {
        cmp("markout_resolved", static_cast<long long>(a.markouts[h].resolved_fills),
            static_cast<long long>(b.markouts[h].resolved_fills));
        cmp("markout_unresolved", static_cast<long long>(a.markouts[h].unresolved_fills),
            static_cast<long long>(b.markouts[h].unresolved_fills));
        cmp("markout_shares", static_cast<long long>(a.markouts[h].shares),
            static_cast<long long>(b.markouts[h].shares));
        cmp("markout_per_share", a.markouts[h].markout_per_share,
            b.markouts[h].markout_per_share);
        cmp("drift_per_share", a.markouts[h].drift_per_share,
            b.markouts[h].drift_per_share);
        cmp("edge_per_share", a.markouts[h].edge_per_share,
            b.markouts[h].edge_per_share);
    }
    return out;
}

// THE INVARIANT. Without feedback the two drivers answer the same question.
void test_no_feedback_matches_the_open_loop_driver_exactly() {
    const Feed feed = build_feed();
    for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
        Backtest<BlindMaker> open(BlindMaker{}, FeeSchedule{});
        ClosedLoopBacktest<BlindInventoryMaker> closed(BlindInventoryMaker{}, m,
                                                       FeeSchedule{});
        for (const auto& msg : feed.msgs) {
            open.on_message(static_cast<char>(msg[0]), msg.data(),
                            static_cast<uint16_t>(msg.size()));
            closed.on_message(static_cast<char>(msg[0]), msg.data(),
                              static_cast<uint16_t>(msg.size()));
        }
        const std::vector<LaneResult> lanes = open.results();
        const LaneResult* want = nullptr;
        for (const LaneResult& l : lanes) {
            if (l.model == m) want = &l;
        }
        CHECK(want != nullptr);
        if (want == nullptr) continue;
        const LaneResult got = closed.result();
        const std::string diff = differences(*want, got);
        if (!diff.empty()) {
            std::fprintf(stderr, "FAIL model %d: %s\n", static_cast<int>(m),
                         diff.c_str());
            ++itchbook::test::failures;
        }
        CHECK_EQ(open.events(), closed.events());
    }
}

// ...and the feed has to actually fill, or the comparison above is two zeroes
// agreeing with each other -- the vacuous-comparison trap phase 7 named.
void test_the_feed_actually_trades() {
    const Feed feed = build_feed();
    ClosedLoopBacktest<BlindInventoryMaker> b(BlindInventoryMaker{}, Model::Optimistic,
                                              FeeSchedule{});
    for (const auto& msg : feed.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
    }
    CHECK(b.result().fills > 0);
    CHECK(b.result().shares > 0);
}

// The feedback path itself: a strategy that records what it is told must see
// its own fills, and the position it is handed must match the harness's.
struct Recorder {
    std::vector<FillEvent> seen;
    std::vector<int64_t> positions;
    int64_t last_view_position = 0;
    void on_event(const InventoryView& v, Ctx& ctx) {
        last_view_position = v.position;
        positions.push_back(v.position);
        int32_t b = 0;
        int32_t a = 0;
        if (!v.tradable || !v.best_bid(&b) || !v.best_ask(&a)) return;
        ctx.quote(next_id_++, Side::Buy, b, 100);
        ctx.quote(next_id_++, Side::Sell, a, 100);
    }
    void on_fill(const FillEvent& f) { seen.push_back(f); }

private:
    uint64_t next_id_ = 1;
};

void test_the_strategy_is_told_its_fills_and_agrees_on_position() {
    const Feed feed = build_feed();
    ClosedLoopBacktest<Recorder> b(Recorder{}, Model::Optimistic, FeeSchedule{});
    for (const auto& msg : feed.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
    }
    const Recorder& s = b.strategy();
    CHECK(!s.seen.empty());                     // it was told something
    CHECK_EQ(s.seen.size(), b.result().fills);  // and told about every fill

    // The position the strategy was handed is the harness's, computed from the
    // same fills. Recomputing it here from the fill stream is the check that
    // the two cannot silently disagree.
    int64_t recomputed = 0;
    for (const FillEvent& f : s.seen) {
        recomputed += (f.side == Side::Buy) ? static_cast<int64_t>(f.shares)
                                            : -static_cast<int64_t>(f.shares);
    }
    CHECK_EQ(recomputed, b.position());
    CHECK_EQ(b.position(), s.last_view_position);
}

// Queue position is the half of the wall that STAYS UP. FillEvent must not
// carry it, and the compiler is the enforcement.
void test_fill_event_withholds_queue_position() {
    static_assert(!exposes_queue_position<FillEvent>,
                  "FillEvent must not expose queue position: it is the estimate "
                  "whose error bars are the subject of phase 6");
    // ...and the field really does exist on the type it was copied from, so the
    // assertion above is testing an omission rather than a typo.
    static_assert(exposes_queue_position<SimFill>,
                  "SimFill should still carry queue position");
    SimFill raw;
    raw.order_id = 7;
    raw.side = Side::Sell;
    raw.price = 1000;
    raw.shares = 250;
    raw.ts = 42;
    raw.liquidity = Liquidity::Added;
    raw.ahead_at_arrival = 9999;
    const FillEvent e = FillEvent::from(raw);
    CHECK_EQ(e.order_id, uint64_t{7});
    CHECK_EQ(e.shares, uint32_t{250});
    CHECK(e.side == Side::Sell);
    CHECK(e.liquidity == Liquidity::Added);
}

// The session clock: configuration, clamped rather than extrapolated, because a
// negative (T - t) makes A-S's spread term imaginary.
void test_session_clock_clamps_outside_the_session() {
    SessionClock c;
    CHECK_EQ(c.elapsed(0), 0.0);
    CHECK_EQ(c.elapsed(c.start_ns), 0.0);
    CHECK_EQ(c.elapsed(c.start_ns - 1), 0.0);
    CHECK_EQ(c.elapsed(c.end_ns), 1.0);
    CHECK_EQ(c.elapsed(c.end_ns + 1000000000ULL), 1.0);
    const double half = c.elapsed((c.start_ns + c.end_ns) / 2);
    CHECK(half > 0.49 && half < 0.51);
    // A degenerate configuration must not divide by zero.
    SessionClock bad{100, 100};
    CHECK_EQ(bad.elapsed(500), 0.0);
}

void test_position_tracker_state_round_trips() {
    PositionTracker t;
    t.on_fill(FillEvent{1, Side::Buy, 100, 500, 0, Liquidity::Added});
    t.on_fill(FillEvent{2, Side::Sell, 101, 200, 0, Liquidity::Removed});
    CHECK_EQ(t.position(), int64_t{300});
    const PositionTracker::State s = t.state();
    PositionTracker fresh;
    CHECK_EQ(fresh.position(), int64_t{0});
    fresh.restore(s);
    CHECK_EQ(fresh.position(), int64_t{300});
    CHECK_EQ(fresh.fills(), uint64_t{2});
}

}  // namespace

int main() {
    test_no_feedback_matches_the_open_loop_driver_exactly();
    test_the_feed_actually_trades();
    test_the_strategy_is_told_its_fills_and_agrees_on_position();
    test_fill_event_withholds_queue_position();
    test_session_clock_clamps_outside_the_session();
    test_position_tracker_state_round_trips();
    return REPORT();
}
