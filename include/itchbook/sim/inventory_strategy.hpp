#pragma once
//
// inventory_strategy.hpp — the feedback wall comes down, and only halfway.
//
// strategy.hpp denies a strategy its fills, its position and its queue
// position, and gives a precise reason for each. Avellaneda-Stoikov cannot live
// behind that wall: its reservation price is
//
//     r(s, q, t) = s - q * gamma * sigma^2 * (T - t)
//
// and `q` is inventory. A strategy that cannot see its own inventory cannot
// implement the model. So the wall comes down -- but only the part of it that
// has to, and the part that stays up is the more interesting one.
//
// WHAT COMES DOWN: fills and position. A strategy is told each of its own fills
// as it happens, and can ask its running share position.
//
// WHAT STAYS UP: queue position, and everything derived from it. `SimFill`
// carries `ahead_at_arrival` -- how many shares were in front of us when we
// joined -- and that field is deliberately NOT copied into FillEvent. It is an
// estimate whose error bars are the entire subject of phase 6, and a strategy
// conditioning on it would be conditioning on the thing being measured. A-S
// needs inventory; it does not need to know where in the queue it sat.
//
// WHAT THE BAND NOW MEANS, WHICH IS THE PARAGRAPH THAT MATTERS.
//
// Phase 6's four-model band is four GRADINGS OF ONE WORLD. One strategy sees
// the market, emits one intent stream, and four fill models score that identical
// stream differently. The band is the disagreement between the models about the
// same decisions, and it is attributable to the models alone -- structurally,
// because there is no feedback path by which the decisions could differ.
//
// That construction is impossible once the strategy sees its fills. A strategy
// in the pessimistic lane gets fewer fills, so it carries different inventory,
// so it quotes differently, so it gets different fills again. The lanes diverge
// from the first fill onward and there is no longer one intent stream to grade.
//
// So the band becomes four CLOSED-LOOP RUNS -- four worlds, each internally
// consistent, each one a complete answer to "what would this strategy have done
// if fills worked like THIS". The band is now over worlds rather than over
// gradings, and it is wider for a reason that is not noise: it includes the
// strategy's own reaction to being filled differently.
//
// Both are legitimate and they answer different questions. Phase 6 asks how much
// the fill model matters to the SCORE of a fixed strategy. Phase 11 asks how
// much it matters to a strategy that reacts. Reporting one while describing the
// other would be the single most misleading thing this project could print, so
// the two live in different headers and produce differently-labelled output.
//
#include <cstdint>
#include <vector>

#include "itchbook/sim/event.hpp"
#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

// One of our own fills, as the strategy is allowed to see it.
//
// Deliberately not SimFill. The omission of `ahead_at_arrival` is the whole
// point and a comment would not enforce it; a separate type does.
struct FillEvent {
    uint64_t order_id = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t shares = 0;
    uint64_t ts = 0;
    // Maker or taker. Kept because it decides the fee, which a strategy
    // choosing between resting and crossing legitimately needs, and because it
    // says nothing about queue position.
    Liquidity liquidity = Liquidity::Added;

    static FillEvent from(const SimFill& f) {
        return FillEvent{f.order_id, f.side, f.price, f.shares, f.ts, f.liquidity};
    }
};

// Everything an inventory-aware strategy may know: the market view it already
// had, plus its own position.
//
// Position and nothing else derived from it. Not equity, not drawdown, not
// realised P&L -- a strategy that wants those can accumulate them from the fill
// stream it is given, and making it do so keeps the interface honest about what
// is a primitive and what is a choice. A-S needs `q` and the clock.
struct InventoryView : MarketView {
    int64_t position = 0;

    // Session progress in [0, 1], which is A-S's (T - t) with the horizon
    // normalised away. Supplied by the harness because the strategy has no way
    // to know when the session ends, and because a strategy computing it from
    // wall clock would produce different answers in a replay than live.
    double session_elapsed = 0.0;
};

// The interface, by convention rather than inheritance -- the same choice
// strategy.hpp and parser.hpp make, for the same reason: dispatch inlines.
//
//   void on_event(const InventoryView&, Ctx&)
//   void on_fill(const FillEvent&)              // optional
//   State state() const / void restore(State)   // optional; see below
//
// A strategy that omits on_fill still works and simply never learns about its
// fills except through `position`, which the harness maintains for it.
//
// ON State/restore, AND THE RULE THAT CONFIGURATION IS NEVER RESTORED.
//
// ledger.hpp, queue_model.hpp and strategy_snapshot.hpp all snapshot what was
// LEARNED and never what was CONFIGURED. A restored ledger brings back cash and
// position; it does not bring back the fee schedule, because the fee schedule is
// the operator's instruction and restoring it would let a stale snapshot
// silently override a corrected one. The same rule applies here and matters
// more: gamma, the volatility window, the horizon and the quote clamps are
// instructions. An inventory estimate and a running variance are state.
//
// A strategy with inventory that cannot survive a restart breaks a property
// phase 7 established, so this is a requirement rather than a nicety.

// A running position, maintained by the harness from the same fills the
// strategy is told about, so the two can never disagree.
//
// It is here rather than inside each strategy because "what is my position" has
// exactly one correct answer and a strategy that tracked it itself could get it
// wrong in a way nothing would catch -- the ledger would say one thing and the
// quotes would be priced off another.
class PositionTracker {
public:
    void on_fill(const FillEvent& f) {
        position_ += (f.side == Side::Buy) ? static_cast<int64_t>(f.shares)
                                           : -static_cast<int64_t>(f.shares);
        ++fills_;
    }
    int64_t position() const { return position_; }
    uint64_t fills() const { return fills_; }

    struct State {
        int64_t position = 0;
        uint64_t fills = 0;
    };
    State state() const { return State{position_, fills_}; }
    void restore(const State& s) {
        position_ = s.position;
        fills_ = s.fills;
    }

private:
    int64_t position_ = 0;
    uint64_t fills_ = 0;
};

}  // namespace itchbook::sim
