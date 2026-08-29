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
#include "itchbook/recover/snapshot.hpp"
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

    // ...but it is not stateless, and that distinction cost an hour. It
    // remembers the touch it last quoted at and the ids it used. A restarted
    // run that does not restore this comes back believing it has quoted
    // nothing: the first event after the cut looks like the touch moved, it
    // requotes, and it mints ids from 1 again -- colliding with orders already
    // resting. The restart test caught it as two extra fills, both `lock`.
    struct State {
        uint64_t next_id = 1;
        int32_t bid = 0;
        int32_t ask = 0;
        uint64_t bid_id = 0;
        uint64_t ask_id = 0;
    };
    State state() const {
        return State{inner.next_id, inner.bid, inner.ask, inner.bid_id, inner.ask_id};
    }
    void restore(const State& s) {
        inner.next_id = s.next_id;
        inner.bid = s.bid;
        inner.ask = s.ask;
        inner.bid_id = s.bid_id;
        inner.ask_id = s.ask_id;
    }
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

// ---- the restart, phase 11's last item -------------------------------------
//
// Run the feed straight through. Run it again, but at message N take a
// snapshot, throw the whole driver away, build a fresh one, restore, and
// continue. The two must end the same.
//
// A LANE could already do this (test_restart.cpp). This driver could not,
// because what it adds over a lane is a strategy that READS ITS OWN FILLS: the
// strategy's state and the tracker feeding it have to travel or the resumed run
// quotes off an inventory it does not hold. And LatencyModel defers every
// intent to a future timestamp, so at the cut there are orders decided and not
// yet arrived -- the piece most easily forgotten and the most damaging to lose,
// because losing it makes the resumed run look TIDIER than the truth.
void test_a_restarted_closed_loop_matches_one_that_never_died() {
    const Feed feed = build_feed();
    // A LATENCY THAT SPANS A WHOLE MESSAGE, deliberately. build_feed() steps
    // 1 ms per message; at the 250 us default an intent decided at one message
    // is still pending at that message's boundary -- test_the_cut_lands_with_
    // orders_in_flight proves exactly that at the default -- but it has arrived
    // before the NEXT message is applied. What the search below needs is
    // stronger: a cut where something in flight is still in flight after the
    // driver has been destroyed and rebuilt. At 2.5 ms a deferred action
    // survives a whole intervening message, and that is what makes dropping
    // `pending_` from restore() detectable at all.
    const LatencyModel slow = LatencyModel::uniform(2500000);

    for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
        // THE CUT IS SEARCHED FOR, NOT PICKED. A restart test is only worth
        // anything if the state it restores is non-trivial at the moment of the
        // cut, and three separate ways of getting that wrong showed up here:
        //
        //   size()/2 landed on a round boundary, where build_feed() has just
        //   deleted all four of its orders and the book is EMPTY -- so the market
        //   half was snapshotted and restored as nothing.
        //
        //   A hand-picked mid-round offset fixed the book but happened to land
        //   with NOTHING IN FLIGHT, and deleting `pending_ = s.pending` from
        //   restore() then left every assertion in this test still passing.
        //
        // So the cut is chosen by requiring all four preconditions at once, which
        // also survives someone editing build_feed() later.
        size_t cut = 0;
        {
            ClosedLoopBacktest<BlindInventoryMaker> probe(BlindInventoryMaker{}, m,
                                                          FeeSchedule{}, slow);
            for (size_t i = 0; i < feed.msgs.size(); ++i) {
                const auto& msg = feed.msgs[i];
                probe.on_message(static_cast<char>(msg[0]), msg.data(),
                                 static_cast<uint16_t>(msg.size()));
                const auto st = probe.state();
                if (!st.ledger.fills.empty() && st.tracker.position != 0 &&
                    !st.pending.empty() && probe.book().best_bid(nullptr)) {
                    cut = i + 1;
                    break;
                }
            }
        }
        CHECK(cut > 0);
        if (cut == 0) continue;

        // (a) the run that never died
        ClosedLoopBacktest<BlindInventoryMaker> whole(BlindInventoryMaker{}, m,
                                                      FeeSchedule{}, slow);
        for (const auto& msg : feed.msgs) {
            whole.on_message(static_cast<char>(msg[0]), msg.data(),
                             static_cast<uint16_t>(msg.size()));
        }

        // (b) the run that died at `cut` and came back
        ClosedLoopBacktest<BlindInventoryMaker> before(BlindInventoryMaker{}, m,
                                                       FeeSchedule{}, slow);
        for (size_t i = 0; i < cut; ++i) {
            const auto& msg = feed.msgs[i];
            before.on_message(static_cast<char>(msg[0]), msg.data(),
                              static_cast<uint16_t>(msg.size()));
        }

        const auto driver_state = before.state();
        const auto strat_state = before.strategy_state();
        const auto book_snap =
            itchbook::recover::capture(before.book(), 0, 0);

        // THE SNAPSHOT MUST BE WORTH TAKING. A cut before anything happened
        // would make the comparison below two fresh objects agreeing, which is
        // the vacuous-comparison trap this file already names once.
        CHECK(driver_state.ledger.fills.size() > 0);
        CHECK(driver_state.tracker.position != 0);
        CHECK(book_snap.orders.size() > 0);
        // AND THE CUT MUST LAND WITH ORDERS IN FLIGHT. Without this the
        // hardest piece of state is never exercised: deleting `pending_ =
        // s.pending` from restore() left every assertion below still passing,
        // because at this cut there happened to be nothing deferred. A guard
        // that the run was EVER in flight is not the same as one that it is in
        // flight HERE, and only the second makes the restore testable.
        CHECK(!driver_state.pending.empty());

        // A genuinely fresh driver: nothing survives except what restore() puts
        // back. If `before` were reused the test would prove nothing at all.
        ClosedLoopBacktest<BlindInventoryMaker> after(BlindInventoryMaker{}, m,
                                                      FeeSchedule{}, slow);
        itchbook::recover::restore(&after.book(), book_snap);
        after.restore(driver_state);
        after.restore_strategy(strat_state);

        for (size_t i = cut; i < feed.msgs.size(); ++i) {
            const auto& msg = feed.msgs[i];
            after.on_message(static_cast<char>(msg[0]), msg.data(),
                             static_cast<uint16_t>(msg.size()));
        }

        const LaneResult w = whole.result();
        const LaneResult a = after.result();

        // The fill path, the position and the money. Markout resolution and
        // kill-switch counters deliberately do NOT travel -- see the banner on
        // ClosedLoopBacktest::State -- so they are not compared here rather
        // than being quietly included and passing by luck.
        CHECK_EQ(a.fills, w.fills);
        CHECK_EQ(a.shares, w.shares);
        CHECK_EQ(a.equity, w.equity);
        CHECK_EQ(a.fees, w.fees);
        CHECK_EQ(a.residual_position, w.residual_position);
        CHECK_EQ(a.lock_fills, w.lock_fills);
        CHECK_EQ(a.through_fills, w.through_fills);
        CHECK_EQ(a.suppressed_quotes, w.suppressed_quotes);
        CHECK_EQ(a.peak_position, w.peak_position);

        // THE INTENSITY INTEGRAL, which LaneResult does not carry and which
        // therefore nothing above would have noticed. tools/calibrate_intensity
        // reads exactly these numbers to fit lambda(delta) for 11.2, and
        // exposure is the denominator: a restarted recorder that came back with
        // started_ = false would charge the first post-restart interval to
        // nobody and quietly move every fitted k.
        const auto& wb = whole.intensity().buckets();
        const auto& ab = after.intensity().buckets();
        CHECK_EQ(ab.size(), wb.size());
        double w_exposure = 0.0;
        double a_exposure = 0.0;
        uint64_t w_bucket_fills = 0;
        for (size_t i = 0; i < wb.size() && i < ab.size(); ++i) {
            CHECK_EQ(ab[i].fills, wb[i].fills);
            CHECK_EQ(ab[i].shares, wb[i].shares);
            w_exposure += wb[i].exposure_seconds;
            a_exposure += ab[i].exposure_seconds;
            w_bucket_fills += wb[i].fills;
        }
        CHECK(a_exposure == w_exposure);
        CHECK_EQ(after.intensity().untradable_ns(), whole.intensity().untradable_ns());
        // ...over an integral that is actually non-zero, and buckets that
        // actually caught fills. Otherwise the three equalities above are
        // 0.0 == 0.0 three times.
        CHECK(w_exposure > 0.0);
        CHECK(w_bucket_fills > 0);

        // ...and the run that never died has to have DONE something, or every
        // equality above is zero == zero.
        CHECK(w.fills > 0);
        CHECK(w.shares > 0);
    }
}

// The in-flight half, on its own, because the assertion above would still pass
// if `pending` were always empty at the cut -- the latency model would simply
// never have anything deferred and the hardest piece of state would go
// untested while looking tested.
void test_the_cut_lands_with_orders_in_flight() {
    const Feed feed = build_feed();
    bool ever_in_flight = false;
    ClosedLoopBacktest<BlindInventoryMaker> b(BlindInventoryMaker{}, Model::Optimistic,
                                              FeeSchedule{});
    for (const auto& msg : feed.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
        if (!b.state().pending.empty()) ever_in_flight = true;
    }
    CHECK(ever_in_flight);
}

int main() {
    test_no_feedback_matches_the_open_loop_driver_exactly();
    test_a_restarted_closed_loop_matches_one_that_never_died();
    test_the_cut_lands_with_orders_in_flight();
    test_the_feed_actually_trades();
    test_the_strategy_is_told_its_fills_and_agrees_on_position();
    test_fill_event_withholds_queue_position();
    test_session_clock_clamps_outside_the_session();
    test_position_tracker_state_round_trips();
    return REPORT();
}
