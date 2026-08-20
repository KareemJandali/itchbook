#pragma once
//
// fees.hpp — exchange and regulatory costs.
//
// The build plan says maker-taker "changes the sign of many strategies", and on
// the validated day it does exactly that. Two things make this file worth its
// own tests rather than a constant in the ledger:
//
//   1. **Regulatory fees are notional-based, not per-share.** SEC Section 31 is
//      charged per dollar of proceeds, so its per-share cost depends on the
//      stock price. On MSFT at $157.57 it is 0.326 cents/share — comparable to
//      the entire $0.0020 base maker rebate. A per-share-only fee struct is
//      structurally incapable of expressing it, and a design that treats it as
//      a small per-share constant is wrong by roughly 8x in the direction that
//      flatters the result.
//
//   2. **Rates change.** Section 31 is reset by the SEC and the maker tiers move.
//      Every rate here is a named, dated default that the report echoes back, so
//      a number produced today is still interpretable later. Nothing is silently
//      baked in.
//
// Sign convention: **positive is a cost, negative is a credit.** A maker rebate
// is negative. This is the opposite of how exchanges publish their schedules and
// it is chosen deliberately, so that total fees add into P&L with a minus sign
// and never need a special case.
//
#include <algorithm>
#include <cstdint>

#include "itchbook/sim/money.hpp"

namespace itchbook::sim {

// Exchange rates, per share, in micro-dollars.
struct ExchangeFees {
    Money maker = -2000;   // $0.0020 credit — NASDAQ base tier, circa 2019
    Money taker = 3000;    // $0.0030 charge
    Money cross = 1500;    // $0.0015 — opening/closing auction
};

// Regulatory rates. Charged on SELLS ONLY, which is not a rounding detail: it
// makes the cost of a round trip depend on which way round it happened.
struct RegulatoryFees {
    // SEC Section 31: per dollar of proceeds, expressed as micro-dollars per
    // dollar. $20.70 per $1,000,000 = 20.70 micro-dollars per dollar.
    // Stored scaled by 100 so the published rate is exact in integers.
    int64_t sec_per_million_dollars_centis = 2070;   // $20.70 -> 2070 hundredths
    // FINRA TAF: per share, with a per-TRADE cap. A sweep that fills as N
    // partials is capped N times, so the cap is applied per fill here and the
    // aggregate is not re-capped.
    Money taf_per_share = 119;                        // $0.000119
    Money taf_cap_per_trade = 5950000;                // $5.95
};

struct FeeSchedule {
    ExchangeFees exchange{};
    RegulatoryFees regulatory{};
    // Echoed into the report so a stored result stays interpretable when the
    // rates move under it.
    const char* name = "nasdaq-base-2019";

    // Exchange fee for one fill, in micro-dollars. Positive is a cost.
    Money exchange_fee(Liquidity liq, uint64_t shares) const {
        const Money rate = liq == Liquidity::Added   ? exchange.maker
                         : liq == Liquidity::Removed ? exchange.taker
                                                     : exchange.cross;
        return rate * static_cast<Money>(shares);
    }

    // Regulatory fee for one fill. Zero unless we sold.
    Money regulatory_fee(bool is_sell, Price4 price, uint64_t shares) const {
        if (!is_sell) return 0;
        const Money proceeds = notional(price, static_cast<int64_t>(shares));  // micros
        // proceeds is in micro-dollars; the rate is centis-micros per dollar.
        // fee_micros = proceeds_micros / 1e6 * rate/100
        const Money sec = divide_round(proceeds * regulatory.sec_per_million_dollars_centis,
                                       100 * kMicrosPerDollar);
        Money taf = regulatory.taf_per_share * static_cast<Money>(shares);
        taf = std::min(taf, regulatory.taf_cap_per_trade);
        return sec + taf;
    }

    Money total_fee(Liquidity liq, bool is_sell, Price4 price, uint64_t shares) const {
        return exchange_fee(liq, shares) + regulatory_fee(is_sell, price, shares);
    }
};

// The top maker tier, for the sensitivity the results doc reports: whether the
// headline strategy is profitable at all can hinge on which tier you assume.
inline FeeSchedule nasdaq_top_tier_2019() {
    FeeSchedule f;
    f.exchange.maker = -3050;   // $0.00305
    f.name = "nasdaq-top-tier-2019";
    return f;
}

// An inverted venue, where the signs flip: you pay to add and are paid to remove.
inline FeeSchedule inverted_venue_2019() {
    FeeSchedule f;
    f.exchange.maker = 1800;
    f.exchange.taker = -1600;
    f.name = "inverted-2019";
    return f;
}

}  // namespace itchbook::sim
