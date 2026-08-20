#pragma once
//
// strategies.hpp — the strategies the phase needs, and why each one exists.
//
// The build plan is explicit: "Run a strategy you know is unprofitable and
// confirm it loses money. A backtester that can't produce a loss is broken."
// Two of the three below have known answers and exist as oracles.
//
#include <cstdint>

#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

// Quotes nothing, ever. Must produce exactly zero fills and exactly zero P&L in
// every model. The cheapest possible check that the harness is not inventing
// trades out of the feed.
struct NullStrategy {
    void on_event(const MarketView&, Ctx&) {}
    static const char* name() { return "null"; }
};

// Symmetric passive market making at the touch: one quote each side, requoted
// whenever the touch moves.
//
// This is the headline strategy, and it is expected to LOSE money on real data.
// The reason is the phase's whole subject: you are filled preferentially when
// the market is about to move against you. Sitting at the bid, you buy from
// sellers who are right, and your quote is still there when the price falls
// because you had no reason to pull it. The rebate is supposed to pay for that,
// and on a passive sell at MSFT's price it does not even cover the regulatory
// fee.
//
// It is also the strategy for which the naive/pessimistic gap is largest, since
// naive assumes you are at the front of every queue you join.
struct TouchMaker {
    uint32_t size = 100;
    uint64_t next_id = 1;

    // Requote only when the touch moves. Requoting on every event would cancel
    // and rejoin constantly, and since a replace loses queue priority the
    // strategy would never get near the front of anything — which would flatter
    // the pessimistic model by making its disadvantage irrelevant.
    int32_t quoted_bid = 0;
    int32_t quoted_ask = 0;
    uint64_t bid_id = 0;
    uint64_t ask_id = 0;

    void on_event(const MarketView& v, Ctx& ctx) {
        if (!v.tradable || !v.mid.ok()) return;
        int32_t bid = 0;
        int32_t ask = 0;
        if (!v.best_bid(&bid) || !v.best_ask(&ask)) return;
        if (bid >= ask) return;   // never quote into a locked or crossed book

        if (bid != quoted_bid) {
            if (bid_id != 0) ctx.cancel(bid_id);
            bid_id = next_id++;
            ctx.quote(bid_id, Side::Buy, bid, size);
            quoted_bid = bid;
        }
        if (ask != quoted_ask) {
            if (ask_id != 0) ctx.cancel(ask_id);
            ask_id = next_id++;
            ctx.quote(ask_id, Side::Sell, ask, size);
            quoted_ask = ask;
        }
    }
    static const char* name() { return "touch-maker"; }
};

// Crosses the spread every N events, alternating side. A known loser with an
// analytically predictable magnitude: it pays the spread plus the taker fee on
// every round trip and captures nothing. If a backtester cannot reproduce that,
// nothing else it reports can be believed.
struct Crosser {
    uint32_t size = 100;
    uint64_t every = 500;
    uint64_t next_id = 1;
    bool buy_next = true;

    void on_event(const MarketView& v, Ctx& ctx) {
        if (!v.tradable || !v.mid.ok()) return;
        if (v.event_index == 0 || v.event_index % every != 0) return;
        int32_t bid = 0;
        int32_t ask = 0;
        if (!v.best_bid(&bid) || !v.best_ask(&ask)) return;
        if (bid >= ask) return;
        if (buy_next) {
            ctx.take(next_id++, Side::Buy, ask, size);
        } else {
            ctx.take(next_id++, Side::Sell, bid, size);
        }
        buy_next = !buy_next;
    }
    static const char* name() { return "crosser"; }
};

// Quotes far from the touch, where nothing can reach it. Must fill essentially
// never; any fills it does get are price-priority artefacts and are worth
// looking at, because they indicate the sim reaching a price the market never
// traded at.
struct FarQuoter {
    uint32_t size = 100;
    int32_t distance_ticks = 200;
    uint64_t next_id = 1;
    bool placed = false;

    void on_event(const MarketView& v, Ctx& ctx) {
        if (placed || !v.tradable || !v.mid.ok()) return;
        int32_t bid = 0;
        if (!v.best_bid(&bid)) return;
        ctx.quote(next_id++, Side::Buy, bid - distance_ticks * 100, size);
        placed = true;
    }
    static const char* name() { return "far-quoter"; }
};

}  // namespace itchbook::sim
