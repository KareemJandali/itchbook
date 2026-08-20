// test_sim — end-to-end backtest properties with analytically known answers.
//
// The build plan is explicit: "Run a strategy you know is unprofitable and
// confirm it loses money. A backtester that can't produce a loss is broken."
// These are the checks that make that statement testable rather than a hope.
//
// Messages are built here rather than read from a file, so the test is
// self-contained and does not depend on a generator that might itself be wrong.
#include <cstdint>
#include <vector>

#include "itchbook/sim/backtest.hpp"
#include "itchbook/sim/strategies.hpp"
#include "tests/check.hpp"

using namespace itchbook::sim;

namespace {

constexpr Price4 MID = 1000000;    // $100.0000
constexpr Price4 CENT = 100;

// ---- a tiny ITCH message builder -------------------------------------------

void be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xff));
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void be64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
std::vector<uint8_t> header(char type, uint64_t ts) {
    std::vector<uint8_t> v{static_cast<uint8_t>(type)};
    be16(v, 1);
    be16(v, 0);
    for (int i = 5; i >= 0; --i) v.push_back(static_cast<uint8_t>(ts >> (8 * i)));
    return v;
}
std::vector<uint8_t> sys_event(uint64_t ts, char code) {
    auto v = header('S', ts);
    v.push_back(static_cast<uint8_t>(code));
    return v;
}
std::vector<uint8_t> add(uint64_t ts, uint64_t ref, char side, uint32_t shares,
                         Price4 price) {
    auto v = header('A', ts);
    be64(v, ref);
    v.push_back(static_cast<uint8_t>(side));
    be32(v, shares);
    for (int i = 0; i < 8; ++i) v.push_back(' ');
    be32(v, static_cast<uint32_t>(price));
    return v;
}
std::vector<uint8_t> exec(uint64_t ts, uint64_t ref, uint32_t shares) {
    auto v = header('E', ts);
    be64(v, ref);
    be32(v, shares);
    be64(v, 1);
    return v;
}

// A small market: a two-cent spread with depth on both sides, then trades.
template <typename Backtester>
void feed_market(Backtester& bt) {
    uint64_t ts = 34200000000000ULL;
    auto send = [&](const std::vector<uint8_t>& v) {
        bt.on_message(static_cast<char>(v[0]), v.data(), static_cast<uint16_t>(v.size()));
    };
    send(sys_event(ts, 'Q'));                 // market open: everything tradable
    uint64_t ref = 1;
    for (int i = 0; i < 5; ++i) {
        ts += 1000000;
        send(add(ts, ref++, 'B', 500, MID - CENT));
        send(add(ts, ref++, 'S', 500, MID + CENT));
    }
    // Some executions against the resting bids, so queue models can advance.
    for (uint64_t r = 1; r <= 9; r += 2) {
        ts += 1000000;
        send(exec(ts, r, 500));
    }
}

void test_a_strategy_that_never_quotes_earns_exactly_nothing() {
    Backtest<NullStrategy> bt{{}, {}};
    feed_market(bt);
    for (const LaneResult& r : bt.results()) {
        CHECK_EQ(r.fills, 0u);
        CHECK_EQ(r.shares, 0u);
        CHECK_EQ(r.equity, 0);
        CHECK_EQ(r.residual_position, 0);
    }
}

void test_crossing_the_spread_pays_exactly_the_half_spread() {
    // The analytic anchor. A taker lifting a two-cent spread pays one cent of
    // half-spread per share, before fees. If `edge` is not exactly -1 cent, the
    // ledger's sign convention or its mid arithmetic is wrong, and nothing else
    // it reports can be trusted.
    FeeSchedule none;
    none.exchange.maker = 0;
    none.exchange.taker = 0;
    none.exchange.cross = 0;
    none.regulatory.sec_per_million_dollars_centis = 0;
    none.regulatory.taf_per_share = 0;

    Crosser c;
    c.size = 100;
    c.every = 3;
    Backtest<Crosser> bt{c, none};
    feed_market(bt);

    const std::vector<LaneResult> rs = bt.results();
    for (const LaneResult& r : rs) {
        if (r.shares == 0) continue;
        const Money edge_ps = divide_round(r.edge, static_cast<Money>(r.shares));
        CHECK_EQ(edge_ps, -10000);   // -1.0000 cents/share, exactly
    }
}

void test_a_taker_loses_money_and_fees_make_it_worse() {
    Crosser c;
    c.size = 100;
    c.every = 3;
    Backtest<Crosser> free_bt{c, [] {
        FeeSchedule f;
        f.exchange.taker = 0;
        f.regulatory.sec_per_million_dollars_centis = 0;
        f.regulatory.taf_per_share = 0;
        return f;
    }()};
    feed_market(free_bt);

    Backtest<Crosser> paid_bt{c, {}};   // the real NASDAQ schedule
    feed_market(paid_bt);

    const auto free_rs = free_bt.results();
    const auto paid_rs = paid_bt.results();
    for (size_t i = 0; i < free_rs.size(); ++i) {
        if (free_rs[i].shares == 0) continue;
        CHECK(free_rs[i].equity < 0);                      // loses on the spread alone
        CHECK(paid_rs[i].equity < free_rs[i].equity);      // and fees deepen it
    }
}

void test_taking_is_identical_in_every_fill_model() {
    // Crossing the spread is not a queue question, so the four models must agree
    // exactly. If they do not, fill-model state is leaking into the taker path.
    Crosser c;
    c.size = 100;
    c.every = 3;
    Backtest<Crosser> bt{c, {}};
    feed_market(bt);
    const auto rs = bt.results();
    for (size_t i = 1; i < rs.size(); ++i) {
        CHECK_EQ(rs[i].equity, rs[0].equity);
        CHECK_EQ(rs[i].shares, rs[0].shares);
        CHECK_EQ(rs[i].fills, rs[0].fills);
    }
}

void test_naive_never_fills_less_than_a_queue_model() {
    // The aggregate form of I5. Naive ignores the queue entirely, so it can only
    // ever fill at least as much as a model that has to wait its turn.
    TouchMaker t;
    t.size = 100;
    Backtest<TouchMaker> bt{t, {}};
    feed_market(bt);
    const auto rs = bt.results();
    uint64_t naive_shares = 0;
    for (const LaneResult& r : rs) {
        if (r.model == Model::Naive) naive_shares = r.shares;
    }
    for (const LaneResult& r : rs) {
        if (r.model == Model::Naive) continue;
        CHECK(r.shares <= naive_shares);
    }
}

void test_a_quote_nobody_can_reach_never_fills() {
    // Far from the touch, with nothing trading through it. Any fill here would
    // mean the simulator reached a price the market never traded at.
    FarQuoter f;
    f.size = 100;
    f.distance_ticks = 500;
    Backtest<FarQuoter> bt{f, {}};
    feed_market(bt);
    for (const LaneResult& r : bt.results()) {
        CHECK_EQ(r.fills, 0u);
        CHECK_EQ(r.equity, 0);
    }
}

void test_nothing_trades_before_the_market_opens() {
    // Without the 'Q' system event the session never opens, so every model must
    // stay flat no matter what the book does.
    TouchMaker t;
    Backtest<TouchMaker> bt{t, {}};
    uint64_t ts = 34200000000000ULL;
    auto send = [&](const std::vector<uint8_t>& v) {
        bt.on_message(static_cast<char>(v[0]), v.data(), static_cast<uint16_t>(v.size()));
    };
    send(add(ts, 1, 'B', 500, MID - CENT));
    send(add(ts, 2, 'S', 500, MID + CENT));
    send(exec(ts + 1, 1, 500));
    for (const LaneResult& r : bt.results()) CHECK_EQ(r.fills, 0u);
}

}  // namespace

int main() {
    test_a_strategy_that_never_quotes_earns_exactly_nothing();
    test_crossing_the_spread_pays_exactly_the_half_spread();
    test_a_taker_loses_money_and_fees_make_it_worse();
    test_taking_is_identical_in_every_fill_model();
    test_naive_never_fills_less_than_a_queue_model();
    test_a_quote_nobody_can_reach_never_fills();
    test_nothing_trades_before_the_market_opens();
    return REPORT();
}
