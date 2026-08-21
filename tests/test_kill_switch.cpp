// test_kill_switch — five limits, one latch.
//
// The build plan asks for a kill switch that "trips on: position limit,
// notional limit, message rate limit, order-to-fill ratio, P&L drawdown. Each
// independently configurable." Independently configurable means each limit must
// fire on its own and stay silent when it is not the one being tested, so every
// case below configures exactly one and asserts the reason by name — a switch
// that trips for the right reason by accident is not a tested switch.
#include <cstdint>

#include "itchbook/risk/kill_switch.hpp"
#include "tests/check.hpp"

using namespace itchbook::risk;
using itchbook::sim::Money;

namespace {

constexpr uint64_t T0 = 34200000000000ULL;   // 09:30:00
constexpr int32_t MARK = 1000000;            // $100.0000
constexpr Money DOLLAR = 1000000;            // micro-dollars in one dollar

void test_nothing_configured_never_trips() {
    // The default must be inert. A limit nobody asked for that halts trading in
    // production is its own outage, and defaults are what people forget to set.
    KillSwitch k;
    for (int i = 0; i < 10000; ++i) {
        k.on_message_sent(T0 + i, true);
        k.observe(T0 + i, 1000000, -999999999, MARK);
    }
    CHECK(k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::None));
}

void test_position_limit() {
    KillSwitchConfig c;
    c.max_position = 1000;
    KillSwitch k{c};

    k.observe(T0, 1000, 0, MARK);
    CHECK(k.live());              // at the limit is not over it

    k.observe(T0 + 1, 1001, 0, MARK);
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Position));
    CHECK_EQ(k.report().observed, 1001);
    CHECK_EQ(k.report().limit, 1000);
}

void test_position_limit_is_absolute() {
    // Short is a position too. A limit that only counts longs is half a limit.
    KillSwitchConfig c;
    c.max_position = 500;
    KillSwitch k{c};
    k.observe(T0, -501, 0, MARK);
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Position));
    CHECK_EQ(k.report().observed, 501);   // reported as magnitude
}

void test_notional_limit_catches_what_shares_miss() {
    // The reason both limits exist. 1,000 shares is 1,000 shares, but at $100 it
    // is $100k of exposure and at $4 it is $4k. A position limit alone prices
    // every symbol the same.
    KillSwitchConfig c;
    c.max_position = 0;                    // OFF: this is the notional's test
    c.max_notional = 50000 * DOLLAR;       // $50,000
    KillSwitch k{c};

    k.observe(T0, 400, MARK, MARK);        // 400 * $100 = $40k
    CHECK(k.live());
    k.observe(T0 + 1, 600, 0, MARK);       // 600 * $100 = $60k
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Notional));
}

void test_message_rate_over_a_bucket_boundary() {
    // The case a fixed one-second window misses: N messages just before a
    // boundary and N just after is 2N in one second, and a counter that resets
    // on the boundary never sees more than N.
    KillSwitchConfig c;
    c.max_messages_per_second = 100;
    KillSwitch k{c};

    // 60 messages in the 100ms bucket ending at T0+500ms...
    for (int i = 0; i < 60; ++i) k.on_message_sent(T0 + 450000000ULL + i, false);
    CHECK(k.live());
    // ...and 60 more just after it. 120 in a window far shorter than a second.
    for (int i = 0; i < 60; ++i) k.on_message_sent(T0 + 550000000ULL + i, false);
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::MessageRate));
}

void test_message_rate_forgets_old_traffic() {
    // ...and the window really does slide. The same 60 messages a full second
    // apart must not accumulate, or the limit degenerates into a daily cap.
    KillSwitchConfig c;
    c.max_messages_per_second = 100;
    KillSwitch k{c};
    for (int burst = 0; burst < 20; ++burst) {
        const uint64_t base = T0 + burst * 2000000000ULL;   // 2s apart
        for (int i = 0; i < 60; ++i) k.on_message_sent(base + i, false);
    }
    CHECK(k.live());
    CHECK(k.message_rate() <= 60);
}

void test_order_to_fill_ratio() {
    KillSwitchConfig c;
    c.max_orders_per_fill = 10;
    c.ratio_min_orders = 100;
    KillSwitch k{c};

    // 99 orders, no fills: under the evidence gate, so no opinion yet.
    for (int i = 0; i < 99; ++i) k.on_message_sent(T0 + i, true);
    CHECK(k.live());

    // The 100th crosses the gate with zero fills behind it.
    k.on_message_sent(T0 + 99, true);
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::OrderToFill));
}

void test_order_to_fill_tolerates_a_strategy_that_actually_trades() {
    // The ratio must not punish a working strategy. 500 orders with 100 fills
    // is 5:1, comfortably inside a 10:1 limit, and has to survive the gate.
    KillSwitchConfig c;
    c.max_orders_per_fill = 10;
    c.ratio_min_orders = 100;
    KillSwitch k{c};
    for (int i = 0; i < 500; ++i) {
        k.on_message_sent(T0 + i, true);
        if (i % 5 == 0) k.on_fill(T0 + i, 0, 0, MARK);
    }
    CHECK(k.live());
}

void test_cancels_count_for_rate_but_not_for_the_ratio() {
    // A cancel is a message the venue has to process, so it counts against the
    // rate. It is not an order, so counting it in the order-to-fill ratio would
    // make every strategy that cancels look like a quote stuffer.
    KillSwitchConfig c;
    c.max_orders_per_fill = 10;
    c.ratio_min_orders = 10;
    KillSwitch k{c};
    for (int i = 0; i < 1000; ++i) k.on_message_sent(T0 + i, false);   // all cancels
    CHECK(k.live());
    CHECK_EQ(k.orders(), 0u);
    CHECK_EQ(k.messages(), 1000u);
}

void test_drawdown_is_peak_to_trough_not_open_to_now() {
    // A strategy that made $10,000 and gave back $9,000 had a bad day, even
    // though it is up $1,000 on the session. Measuring from the open would call
    // that a good day and keep trading.
    KillSwitchConfig c;
    c.max_drawdown = 5000 * DOLLAR;        // $5,000
    KillSwitch k{c};

    k.observe(T0, 0, 10000 * DOLLAR, MARK);        // peak: +$10,000
    CHECK(k.live());
    k.observe(T0 + 1, 0, 6000 * DOLLAR, MARK);     // -$4,000 off the peak
    CHECK(k.live());
    CHECK_EQ(k.drawdown(), 4000 * DOLLAR);
    k.observe(T0 + 2, 0, 4000 * DOLLAR, MARK);     // -$6,000 off the peak
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Drawdown));
    CHECK_EQ(k.report().observed, 6000 * DOLLAR);
}

void test_the_latch_holds_and_keeps_the_first_cause() {
    // Everything after a trip is a consequence of not having stopped. If a later
    // breach overwrote the record, the post-mortem would find the aftermath and
    // not the cause.
    KillSwitchConfig c;
    c.max_position = 100;
    c.max_drawdown = 1000 * DOLLAR;
    KillSwitch k{c};

    k.observe(T0, 101, 0, MARK);                       // position trips first
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Position));
    const uint64_t first_ts = k.report().ts;

    k.observe(T0 + 1000, 0, -50000 * DOLLAR, MARK);    // drawdown would too
    CHECK(!k.live());
    CHECK_EQ(static_cast<int>(k.report().reason), static_cast<int>(Trip::Position));
    CHECK_EQ(k.report().ts, first_ts);

    // Recovering to a safe state does NOT re-arm it. This is the whole point.
    k.observe(T0 + 2000, 0, 0, MARK);
    CHECK(!k.live());

    // Only an explicit operator reset does.
    k.reset();
    CHECK(k.live());
}

// ---- the sixth limit: a queue that does not drain --------------------------
//
// Every other limit here is about what the system DID. This one is about what
// it KNOWS, and its whole design is in the word "sustained": a ring that fills
// and drains is a ring doing its job, and only a backlog that stays above the
// line is a consumer failing to keep up.

const uint64_t kMs = 1000000ULL;

void test_ring_occupancy_disabled_by_default() {
    KillSwitch k;
    for (int i = 0; i < 100; ++i) {
        k.on_ring_occupancy(static_cast<uint64_t>(i) * kMs, 1000000);
    }
    CHECK(k.live());
}

void test_a_ring_below_the_line_never_trips() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 100 * kMs;
    KillSwitch k(cfg);
    for (int i = 0; i < 10000; ++i) {
        k.on_ring_occupancy(static_cast<uint64_t>(i) * kMs, 1000);   // AT the line
    }
    CHECK(k.live());
    CHECK(!k.backlogged());
    CHECK_EQ(k.peak_ring_occupancy(), uint64_t{1000});
}

// The case the duration exists for. A deep burst that drains is normal: phase
// 10.7 peaked above 51,000 of 65,536 slots at rates that lost nothing.
void test_bursts_that_drain_never_trip_however_deep() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 100 * kMs;
    KillSwitch k(cfg);
    uint64_t t = 0;
    for (int burst = 0; burst < 50; ++burst) {
        // 90 ms above the line — nearly the whole window — then a dip.
        for (int i = 0; i < 9; ++i) {
            t += 10 * kMs;
            k.on_ring_occupancy(t, 60000);
        }
        t += 10 * kMs;
        k.on_ring_occupancy(t, 5);        // drained: the clock restarts
    }
    CHECK(k.live());
    CHECK_EQ(k.peak_ring_occupancy(), uint64_t{60000});
}

void test_a_backlog_that_does_not_drain_trips() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 100 * kMs;
    KillSwitch k(cfg);
    uint64_t t = 0;
    k.on_ring_occupancy(t, 4000);
    CHECK(k.live());               // the breach alone is not the trip
    CHECK(k.backlogged());
    for (int i = 0; i < 9; ++i) {  // 90 ms: still inside the window
        t += 10 * kMs;
        k.on_ring_occupancy(t, 4000);
    }
    CHECK(k.live());
    t += 10 * kMs;                 // 100 ms: the window closes
    k.on_ring_occupancy(t, 4000);
    CHECK(!k.live());
    CHECK(k.report().reason == Trip::FeedBacklog);
    CHECK_EQ(k.report().observed, int64_t{4000});
    CHECK_EQ(k.report().limit, int64_t{1000});
    CHECK_EQ(k.report().ts, t);
}

// A dip resets the clock rather than pausing it. Otherwise a ring that spends
// 99% of its time backlogged would never trip as long as it touched the line
// occasionally, which is exactly the system this limit is for.
void test_a_dip_restarts_the_clock_rather_than_pausing_it() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 100 * kMs;
    KillSwitch k(cfg);
    uint64_t t = 0;
    for (int i = 0; i < 9; ++i) {     // 90 ms above
        k.on_ring_occupancy(t, 4000);
        t += 10 * kMs;
    }
    k.on_ring_occupancy(t, 0);        // one dip
    t += 10 * kMs;
    for (int i = 0; i < 9; ++i) {     // 90 ms above again
        k.on_ring_occupancy(t, 4000);
        t += 10 * kMs;
    }
    CHECK(k.live());                  // 180 ms above in total, never 100 in a row
}

void test_a_zero_window_trips_on_the_breach_itself() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 0;
    KillSwitch k(cfg);
    k.on_ring_occupancy(500, 1000);
    CHECK(k.live());                  // at the line, not over it
    k.on_ring_occupancy(500, 1001);
    CHECK(!k.live());
    CHECK(k.report().reason == Trip::FeedBacklog);
    CHECK_EQ(k.report().observed, int64_t{1001});
}

// A clock that goes backwards is a caller bug. Unsigned subtraction would turn
// it into an instant trip, and a kill switch that latches on a stale timestamp
// is one nobody leaves enabled.
void test_a_clock_that_goes_backwards_restarts_rather_than_tripping() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 100 * kMs;
    KillSwitch k(cfg);
    k.on_ring_occupancy(500 * kMs, 4000);
    k.on_ring_occupancy(1 * kMs, 4000);      // backwards
    CHECK(k.live());
    k.on_ring_occupancy(50 * kMs, 4000);     // 49 ms from the restart
    CHECK(k.live());
    k.on_ring_occupancy(101 * kMs, 4000);    // 100 ms from the restart
    CHECK(!k.live());
}

// ...and it latches like every other limit, keeping the first cause.
void test_the_backlog_trip_latches_and_survives_the_queue_draining() {
    KillSwitchConfig cfg;
    cfg.max_ring_occupancy = 1000;
    cfg.backlog_sustained_ns = 0;
    KillSwitch k(cfg);
    k.on_ring_occupancy(kMs, 9999);
    CHECK(!k.live());
    for (int i = 0; i < 100; ++i) {
        k.on_ring_occupancy((2 + static_cast<uint64_t>(i)) * kMs, 0);
    }
    CHECK(!k.live());
    CHECK(k.report().reason == Trip::FeedBacklog);
    CHECK_EQ(k.report().observed, int64_t{9999});
    k.reset();
    CHECK(k.live());
    CHECK(!k.backlogged());
}

}  // namespace

int main() {
    test_nothing_configured_never_trips();
    test_position_limit();
    test_position_limit_is_absolute();
    test_notional_limit_catches_what_shares_miss();
    test_message_rate_over_a_bucket_boundary();
    test_message_rate_forgets_old_traffic();
    test_order_to_fill_ratio();
    test_order_to_fill_tolerates_a_strategy_that_actually_trades();
    test_cancels_count_for_rate_but_not_for_the_ratio();
    test_drawdown_is_peak_to_trough_not_open_to_now();
    test_the_latch_holds_and_keeps_the_first_cause();
    test_ring_occupancy_disabled_by_default();
    test_a_ring_below_the_line_never_trips();
    test_bursts_that_drain_never_trip_however_deep();
    test_a_backlog_that_does_not_drain_trips();
    test_a_dip_restarts_the_clock_rather_than_pausing_it();
    test_a_zero_window_trips_on_the_breach_itself();
    test_a_clock_that_goes_backwards_restarts_rather_than_tripping();
    test_the_backlog_trip_latches_and_survives_the_queue_draining();
    return REPORT();
}
