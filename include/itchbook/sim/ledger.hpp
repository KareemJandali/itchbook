#pragma once
//
// ledger.hpp — position, cash, and where the money actually came from.
//
// No `double` anywhere in the accounting path. Micro-dollars in int64: a day's
// notional is around 1e14 against an int64 range of 9.2e18, so the headroom is
// not the reason — exactness is. A P&L that drifts by a rounding error per fill
// over a million fills is not a P&L.
//
// The authoritative number is `equity(mark) = cash + position * mark`, with all
// fees folded in. It decomposes into THREE terms, and the third is not optional:
//
//     equity(mark) == edge + drift(mark) + fees
//
//     edge        what we captured against the mid at the moment we filled
//     drift(mark) where the mid went afterwards — the adverse-selection term
//     fees        the exchange and regulatory stream
//
// Defining `holding = equity - edge` instead would dump the whole fee and rebate
// stream into the drift term, which is exactly the number this phase is trying
// to measure. Maker-taker is large enough to change the sign of the result, so
// it gets its own line.
//
// `edge` is NOT invariant across fill models, and claiming it is would be
// falsified by the first real run: the models fill at different times, so the
// same resting price meets a different mid. The gap between models is itself a
// result — it says how far the touch had moved before each model filled us.
//
#include <cstdint>
#include <vector>

#include "itchbook/sim/event.hpp"
#include "itchbook/sim/fees.hpp"
#include "itchbook/sim/mid.hpp"
#include "itchbook/sim/money.hpp"

namespace itchbook::sim {

struct LedgerFill {
    uint64_t ts = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t shares = 0;
    int64_t two_mid = 0;      // prevailing doubled mid at fill time
    bool mid_ok = false;
    Money fee = 0;
    Liquidity liquidity = Liquidity::Added;
};

class Ledger {
public:
    explicit Ledger(FeeSchedule schedule = {}) : schedule_(schedule) {}

    // `mid` is the prevailing mid at the moment of the fill, used for the edge
    // term. A fill with no usable mid still moves cash and position; it is
    // excluded from the edge/drift decomposition and COUNTED, because silently
    // dropping it would make the reconciliation identity hold by construction
    // and therefore prove nothing.
    void on_fill(const SimFill& f, const Mid& mid) {
        const int64_t signed_shares =
            f.side == Side::Buy ? static_cast<int64_t>(f.shares)
                                : -static_cast<int64_t>(f.shares);
        const Money fee = schedule_.total_fee(f.liquidity, f.side == Side::Sell,
                                              f.price, f.shares);

        cash_ -= notional(f.price, signed_shares);   // buying costs cash
        cash_ -= fee;                                // positive fee is a cost
        position_ += signed_shares;
        fees_total_ += fee;

        LedgerFill lf;
        lf.ts = f.ts;
        lf.side = f.side;
        lf.price = f.price;
        lf.shares = f.shares;
        lf.two_mid = mid.two_mid;
        lf.mid_ok = mid.ok();
        lf.fee = fee;
        lf.liquidity = f.liquidity;
        fills_.push_back(lf);

        if (mid.ok()) {
            const int64_t sgn = f.side == Side::Buy ? 1 : -1;
            // Positive is in our favour: a buy below the mid captured half-spread.
            two_edge_ += sgn * (mid.two_mid - 2 * static_cast<int64_t>(f.price)) *
                         static_cast<int64_t>(f.shares);
        } else {
            ++fills_without_mid_;
        }
    }

    // ---- the authoritative number ----

    Money equity(Price4 mark) const { return cash_ + notional(mark, position_); }

    Money cash() const { return cash_; }
    int64_t position() const { return position_; }
    Money fees() const { return fees_total_; }   // positive is a cost

    // Half-spread captured against the mid at fill time.
    Money edge() const { return two_edge_ * (kMicrosPerPrice4 / 2); }

    // Where the mid went between each fill and the mark: the adverse-selection
    // term. Negative means we were picked off.
    Money drift(int64_t two_mark) const {
        int64_t total = 0;
        for (const LedgerFill& f : fills_) {
            if (!f.mid_ok) continue;
            const int64_t sgn = f.side == Side::Buy ? 1 : -1;
            total += sgn * (two_mark - f.two_mid) * static_cast<int64_t>(f.shares);
        }
        return total * (kMicrosPerPrice4 / 2);
    }

    // equity == edge + drift - fees, exactly, when every fill had a usable mid
    // and the mark is that same doubled mid halved. Reported rather than
    // asserted: the exclusions are real, and hiding them would turn a check into
    // a tautology. A tautological identity billed as an oracle is worse than no
    // oracle, because it looks like one.
    Money reconciliation_error(int64_t two_mark) const {
        const Price4 mark = static_cast<Price4>(two_mark / 2);
        return equity(mark) - (edge() + drift(two_mark) - fees_total_);
    }

    const std::vector<LedgerFill>& fills() const { return fills_; }
    uint64_t fills_without_mid() const { return fills_without_mid_; }

    uint64_t shares_traded() const {
        uint64_t total = 0;
        for (const LedgerFill& f : fills_) total += f.shares;
        return total;
    }

    // Every headline figure is per share. Totals scale with fill count, so a
    // model that fills more looks better on a total and the comparison between
    // fill models stops meaning anything.
    Money per_share(Money total) const {
        const uint64_t shares = shares_traded();
        return shares == 0 ? 0 : divide_round(total, static_cast<Money>(shares));
    }

    // ---- state, for a mid-day restart --------------------------------------
    //
    // The build plan's phase 7 asks to "reconstruct position and open orders
    // after a mid-day process restart". Position is this class. The fee
    // schedule is deliberately NOT part of the state: it is configuration, it
    // comes from the command line on both sides of the restart, and restoring
    // it from a file would let a snapshot silently override the schedule the
    // operator asked for.
    //
    // The fill log travels with it. Position and cash alone would restore the
    // number that matters and quietly change every derived one — drift()
    // integrates over fills, and per_share() divides by their total — so a
    // restart would report a different edge/drift decomposition for the same
    // day and nothing would say why.
    struct State {
        Money cash = 0;
        int64_t position = 0;
        Money fees_total = 0;
        int64_t two_edge = 0;
        uint64_t fills_without_mid = 0;
        std::vector<LedgerFill> fills;
    };

    State state() const {
        return State{cash_, position_, fees_total_, two_edge_, fills_without_mid_, fills_};
    }

    void restore(const State& s) {
        cash_ = s.cash;
        position_ = s.position;
        fees_total_ = s.fees_total;
        two_edge_ = s.two_edge;
        fills_without_mid_ = s.fills_without_mid;
        fills_ = s.fills;
    }

private:
    FeeSchedule schedule_;
    Money cash_ = 0;
    int64_t position_ = 0;
    Money fees_total_ = 0;
    int64_t two_edge_ = 0;
    uint64_t fills_without_mid_ = 0;
    std::vector<LedgerFill> fills_;
};

}  // namespace itchbook::sim
