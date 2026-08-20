// test_fees — fee arithmetic against hand-computed goldens at a real price.
//
// This file exists because two of the phase's most dangerous errors are here,
// both silent, both flattering:
//
//   * SEC Section 31 treated as a small per-share constant. It is charged per
//     DOLLAR of proceeds, so its per-share cost scales with the stock price.
//     Getting it wrong by ~8x is easy and moves the headline.
//   * A fee unit of tenths of a cent, in which a maker rate of -20 reads as
//     $0.0020 and is actually $0.020 — ten times the real rebate, four times the
//     entire gross edge of a one-cent spread.
//
// Every number below is derived by hand in the comment above it, from the
// published rate, at MSFT's validated closing price of $157.57.
#include <cstdint>

#include "itchbook/sim/fees.hpp"
#include "tests/check.hpp"

using namespace itchbook::sim;

namespace {

constexpr Price4 kMsft = 1575700;   // $157.5700, the validated 2019-12-30 close

void test_price_scale_is_exact() {
    // Price(4) is 1e-4 USD, Money is 1e-6 USD. One tick is exactly 100 micros.
    CHECK_EQ(notional(10000, 1), 1000000);       // $1.00 x 1 share = 1e6 micros
    CHECK_EQ(notional(kMsft, 100), 15757000000); // 100 x $157.57 = $15,757
    CHECK_EQ(notional(kMsft, -100), -15757000000);
    // The $199,999.99 stub quote from the validated day, times a round lot,
    // is already 2e12 micros — this is why Money is int64.
    CHECK_EQ(notional(1999999900, 100), 19999999000000);
}

void test_maker_rebate_is_a_credit() {
    FeeSchedule f;
    // Base tier $0.0020/share credit. 100 shares -> -$0.20 -> -200,000 micros.
    // If this reads -2,000,000 the unit is wrong by 10x.
    CHECK_EQ(f.exchange_fee(Liquidity::Added, 100), -200000);
    CHECK_EQ(f.exchange_fee(Liquidity::Removed, 100), 300000);   // $0.0030 charge
    CHECK_EQ(f.exchange_fee(Liquidity::Cross, 100), 150000);     // $0.0015
}

void test_section31_is_notional_based() {
    FeeSchedule f;
    // $20.70 per $1,000,000 of proceeds.
    // 100 shares x $157.57 = $15,757 proceeds.
    // 15757/1e6 x 20.70 = $0.32617 -> 326,170 micros (SEC portion).
    // FINRA TAF: $0.000119 x 100 = $0.0119 -> 11,900 micros.
    // Total regulatory = 338,070 micros.
    CHECK_EQ(f.regulatory_fee(true, kMsft, 100), 338070);

    // Per share that is 3,380.7 micros = 0.338 cents/share. A per-share constant
    // of ~0.04 cents — the figure a mils-based schedule tends to produce — would
    // be wrong by roughly 8x.
    const double cps = to_cents_per_share(338070 / 100);
    CHECK(cps > 0.33 && cps < 0.34);
}

void test_regulatory_fees_are_sells_only() {
    FeeSchedule f;
    CHECK_EQ(f.regulatory_fee(false, kMsft, 100), 0);
    CHECK(f.regulatory_fee(true, kMsft, 100) > 0);
    // So a round trip costs differently depending on which leg is the sell,
    // and a model that charges both legs doubles a real asymmetry.
}

void test_section31_scales_with_price() {
    FeeSchedule f;
    // Same share count, ten times the price, ten times the SEC fee. The TAF
    // component is per share and does not move.
    const Money at_low = f.regulatory_fee(true, 100000, 100);    // $10.00
    const Money at_high = f.regulatory_fee(true, 1000000, 100);  // $100.00
    const Money taf = 119 * 100;
    CHECK_EQ((at_high - taf), 10 * (at_low - taf));
}

void test_taf_cap_binds() {
    FeeSchedule f;
    auto sec = [](Price4 px, uint64_t sh) {
        return divide_round(notional(px, static_cast<int64_t>(sh)) * 2070,
                            100 * kMicrosPerDollar);
    };
    // $0.000119/share reaches the $5.95 cap at exactly 50,000 shares.
    CHECK_EQ(119 * 50000, 5950000);
    CHECK_EQ(f.regulatory_fee(true, 10000, 50000), sec(10000, 50000) + 5950000);
    // Past it, TAF stops growing while the notional-based SEC portion keeps going.
    CHECK_EQ(f.regulatory_fee(true, 10000, 60000), sec(10000, 60000) + 5950000);
    CHECK(sec(10000, 60000) > sec(10000, 50000));
}

void test_the_rebate_does_not_survive_a_sell() {
    // The headline consequence, and the reason regulatory fees cannot be folded
    // into a per-share exchange rate: on a passive SELL at MSFT's price, the
    // $0.0020 maker rebate is a 200,000 micro credit and the regulatory charge
    // is 338,070 micros. The "rebate" is a net cost of 138,070 micros.
    FeeSchedule f;
    const Money sell = f.total_fee(Liquidity::Added, true, kMsft, 100);
    CHECK_EQ(sell, 138070);
    CHECK(sell > 0);   // positive is a cost

    // The same passive fill on the BUY side is a genuine credit.
    const Money buy = f.total_fee(Liquidity::Added, false, kMsft, 100);
    CHECK_EQ(buy, -200000);
    CHECK(buy < 0);
}

void test_tier_and_venue_alternatives() {
    // Top tier: $0.00305 credit rather than $0.0020.
    CHECK_EQ(nasdaq_top_tier_2019().exchange_fee(Liquidity::Added, 100), -305000);
    // Inverted venue: you pay to add and are paid to remove.
    const FeeSchedule inv = inverted_venue_2019();
    CHECK(inv.exchange_fee(Liquidity::Added, 100) > 0);
    CHECK(inv.exchange_fee(Liquidity::Removed, 100) < 0);
}

void test_rounding_is_symmetric() {
    // A loss must round the same distance as the mirrored gain, or totals drift
    // in one direction over millions of fills.
    CHECK_EQ(divide_round(5, 2), 3);
    CHECK_EQ(divide_round(-5, 2), -3);
    CHECK_EQ(divide_round(0, 7), 0);
    CHECK_EQ(divide_round(7, 0), 0);
}

}  // namespace

int main() {
    test_price_scale_is_exact();
    test_maker_rebate_is_a_credit();
    test_section31_is_notional_based();
    test_regulatory_fees_are_sells_only();
    test_section31_scales_with_price();
    test_taf_cap_binds();
    test_the_rebate_does_not_survive_a_sell();
    test_tier_and_venue_alternatives();
    test_rounding_is_symmetric();
    return REPORT();
}
