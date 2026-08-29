#pragma once
//
// parity_maker.hpp — lane A's mirror of the live strategy, for the 12.9 A/B.
//
// WHY THIS EXISTS AND WHY IT COPIES THE SIMPLER SIDE. Phase 12.9 wants one
// strategy run through two harnesses. The two that exist differ in every
// dimension that matters: TouchMaker quotes both sides at the touch, requotes
// when the touch moves, cancels to do it, and refuses a crossed book;
// tools/strategy.cpp quotes buy-only two ticks under the bid every N messages,
// never cancels, and caps into a crossed book rather than standing aside. On
// MSFT 2019-12-30 that is 9,892 fills against 2,125 -- 7x apart on shares. A
// diff of those two would measure the strategies, not the harnesses.
//
// So lane A moves to lane B, and NOT the reverse. tools/strategy.cpp's
// behaviour is what phase 12.7 proved correct and what 12.8 measured to the
// nanosecond; editing it to meet the backtester would invalidate both, and the
// live loop is the constrained side anyway -- it cannot cancel, cannot quote
// two-sided, and cannot see a session state it never subscribed to.
//
// WHAT IS FAITHFULLY MIRRORED, from tools/strategy.cpp:669-690:
//
//   * buy only. The live loop hardcodes 'B'.
//   * price = best_bid - offset_ticks*tick, with the SAME crossed-book cap:
//     if (ask - tick < base) base = ask - tick. On a coherent book that is a
//     no-op; on a crossed one it is the difference between resting and
//     crossing, and an order that crosses is one the tape can never show us as
//     a maker fill.
//   * quote when (events since last quote) >= stride, capped by max_orders.
//   * last_quote_at advances ONLY when a quote is actually sent. A blocked
//     quote retries on the next event rather than burning its turn.
//   * never cancels. There is no cancel path in the live strategy at all;
//     ouch::encode::cancel_order exists and is dead code.
//   * NO session gate and NO mid requirement. TouchMaker returns early unless
//     v.tradable && v.mid.ok(); the live strategy tracks neither, so mirroring
//     it means deliberately NOT gating here. Adding the gate would be the more
//     correct strategy and the wrong mirror.
//
// WHAT CANNOT BE MIRRORED, and is therefore a finding rather than a bug:
//
//   * THE TRIGGER COUNTER. The live strategy counts `applied` -- messages that
//     mutated its book (strategy.cpp:337) -- while MarketView exposes
//     event_index, which counts every message the backtester saw
//     (backtest.hpp:221). The two differ by whatever fraction of the tape is
//     non-mutating, so the same `stride` does not put quotes at the same
//     instants. Calibrating stride until the order COUNTS match would hide
//     that behind a fitted constant; it is left visible and reported instead.
//   * OUR OWN ORDERS IN THE BOOK. Lane B's exchange publishes the strategy's
//     own 'A' and the strategy applies it, so best_bid() can be our own order
//     (1,180 of 1,200 own adds were seen on the feed). Lane A's book is built
//     from the feed alone. Quoting BELOW the touch is what keeps this mostly
//     inert -- an order two ticks under the bid does not become the bid unless
//     the market falls to it -- which is why the mirror keeps the offset rather
//     than joining the touch.
//
#include <cstdint>

#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

struct ParityMaker {
    // Defaults are tools/strategy.cpp's defaults, so an unconfigured
    // ParityMaker is the live strategy as it actually ran.
    uint32_t size = 100;            // --quote-shares
    uint64_t stride = 200;          // --quote-every
    int32_t offset_ticks = 2;       // --quote-offset-ticks
    int32_t tick = 100;             // --tick, in Price4 units
    uint64_t max_orders = 2000;     // --max-orders

    uint64_t next_id = 1;
    uint64_t orders_sent = 0;
    uint64_t last_quote_at = 0;
    bool armed = false;

    void on_event(const MarketView& v, Ctx& ctx) {
        if (orders_sent >= max_orders) return;
        if (!armed) {                       // first event anchors the counter
            last_quote_at = v.event_index;
            armed = true;
            return;
        }
        if (v.event_index - last_quote_at < stride) return;

        int32_t bid = 0;
        int32_t ask = 0;
        int32_t base = 0;
        if (v.best_bid(&bid)) {
            base = bid;
            // The crossed-book cap, verbatim from strategy.cpp:687.
            if (v.best_ask(&ask) && ask - tick < base) base = ask - tick;
        }
        const int32_t px = base > 0 ? base - offset_ticks * tick : 0;
        if (px <= 0) return;   // no quote, and the counter does NOT advance

        last_quote_at = v.event_index;
        ++orders_sent;
        ctx.quote(next_id++, Side::Buy, px, size);
    }

    static const char* name() { return "parity-maker"; }
};

}  // namespace itchbook::sim
