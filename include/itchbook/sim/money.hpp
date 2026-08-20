#pragma once
//
// money.hpp — the unit everything in the simulator is counted in.
//
// One integer type, one scale, chosen so that no published rate needs rounding
// and no realistic quantity overflows.
//
//   * **Micro-dollars (1e-6 USD).** Every rate this phase deals with lands on an
//     exact integer: $0.00300 is 3000, $0.00305 is 3050, $0.000119 is 119.
//     Tenths of a cent ("mils") cannot express the TAF rate at all, and worse,
//     invites reading a maker rate of -20 as $0.0020 when it is $0.020 — a 10x
//     error, in the flattering direction, four times the gross edge of a
//     one-cent spread.
//
//   * **int64, not int32.** The validated MSFT day carries a stub quote at
//     $199,999.99, which is 1,999,999,900 in Price(4) and already most of an
//     int32. Notional over a day is far past it.
//
// Prices stay in Price(4) — 1e-4 USD — because that is what the wire uses and
// the book stores. The conversion is exact and one-directional: a Price(4) tick
// is 100 micro-dollars.
//
#include <cstdint>

namespace itchbook::sim {

using Money = int64_t;                     // 1e-6 USD
using Price4 = int32_t;                    // 1e-4 USD, as on the wire

constexpr Money kMicrosPerPrice4 = 100;    // exact
constexpr Money kMicrosPerDollar = 1000000;
constexpr Money kMicrosPerCent = 10000;

// Which side of the trade we were. A passive fill earns the rebate; an
// aggressive one pays the fee. Getting this backwards is worth about a cent a
// share, which is the entire edge of a typical spread.
enum class Liquidity : uint8_t {
    Added,     // we were the resting order — maker
    Removed,   // we crossed the spread — taker
    Cross,     // we participated in an opening or closing auction
};

// Signed notional of a fill, in micro-dollars. Shares are unsigned on the wire
// but a sell is negative cash flow here, so the caller passes signed shares.
inline Money notional(Price4 price, int64_t signed_shares) {
    return static_cast<Money>(price) * kMicrosPerPrice4 * signed_shares;
}

// Round-half-away-from-zero, so that a negative total rounds the same distance
// as its positive mirror. Truncation toward zero would make a loss look
// systematically smaller than the equivalent gain.
inline Money divide_round(Money numerator, Money denominator) {
    if (denominator == 0) return 0;
    const Money half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator
                          : (numerator - half) / denominator;
}

// Micro-dollars per share -> cents per share, the unit fee schedules are quoted
// in, so that a markout of 0.30 c/share nets directly against a $0.0030 rebate.
inline double to_cents_per_share(Money micros_per_share) {
    return static_cast<double>(micros_per_share) / 10000.0;
}

}  // namespace itchbook::sim
