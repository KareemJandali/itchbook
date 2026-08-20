#pragma once
//
// markout.hpp — adverse selection, quantified.
//
// The build plan: "After you fill, where does the mid go over the next 100ms /
// 1s / 10s? If it consistently moves against you, you're being picked off —
// quantify it."
//
// Definition, for a fill of q shares at price F with sgn = +1 for a buy:
//
//     markout(h) = edge + drift(h)
//     edge       = sgn * (M(T)     - F)        half-spread captured, or paid
//     drift(h)   = sgn * (M(T + h) - M(T))     the pure adverse-selection term
//
// **Positive is in our favour in every column, always.** Negative means we were
// picked off. Adverse selection is conventionally quoted with the opposite sign,
// so `price_impact()` returns -drift for comparison against published Rule 605
// figures.
//
// The fill models are compared on DRIFT, not on markout. Edge is not invariant
// across them and claiming it would be falsified by the first real run: the
// models fill at different times, so the same resting price meets a different
// mid. That gap is itself a result — it says how far the touch had moved before
// each model filled us.
//
// Horizons are resolved by replaying forward, not by storing every mid: a fill
// registers a pending observation, and the engine stamps it the first time the
// clock passes T+h. A fill whose horizon runs past the end of the session is
// left UNRESOLVED and counted, never silently dropped — dropping them
// conditions the sample on surviving to the end of the day, which is exactly
// the population most likely to have been picked off.
//
#include <algorithm>
#include <cstdint>
#include <vector>

#include "itchbook/sim/event.hpp"
#include "itchbook/sim/mid.hpp"
#include "itchbook/sim/money.hpp"

namespace itchbook::sim {

constexpr uint64_t kMs = 1000000ULL;         // nanoseconds in a millisecond
constexpr uint64_t kHorizons[] = {100 * kMs, 1000 * kMs, 10000 * kMs};
constexpr size_t kNumHorizons = sizeof(kHorizons) / sizeof(kHorizons[0]);

struct Sample {
    uint64_t ts = 0;
    Side side = Side::Buy;
    uint32_t shares = 0;
    int64_t two_fill = 0;      // 2 * fill price
    int64_t two_mid_at_fill = 0;
    int64_t two_mid_at[kNumHorizons] = {0, 0, 0};
    bool resolved[kNumHorizons] = {false, false, false};
};

struct HorizonReport {
    uint64_t horizon_ns = 0;
    uint64_t resolved_fills = 0;
    uint64_t unresolved_fills = 0;
    uint64_t shares = 0;
    // Share-weighted, in micro-dollars per share. Positive is in our favour.
    Money drift_per_share = 0;
    Money edge_per_share = 0;
    Money markout_per_share = 0;

    // The sign convention published Rule 605 numbers use.
    Money price_impact_per_share() const { return -drift_per_share; }
};

class MarkoutEngine {
public:
    // Register a fill. `mid` must be the prevailing mid at fill time; a fill
    // without one contributes nothing and is counted.
    void on_fill(const SimFill& f, const Mid& mid) {
        if (!mid.ok()) {
            ++fills_without_mid_;
            return;
        }
        Sample s;
        s.ts = f.ts;
        s.side = f.side;
        s.shares = f.shares;
        s.two_fill = 2 * static_cast<int64_t>(f.price);
        s.two_mid_at_fill = mid.two_mid;
        samples_.push_back(s);
    }

    // Called as the clock advances. Stamps any pending horizon whose deadline
    // has passed. Only usable mids stamp: during a halt the mid is not
    // meaningful, so the observation stays pending rather than recording a
    // wrong number.
    void observe(uint64_t now_ns, const Mid& mid) {
        if (!mid.ok()) return;
        // Samples are appended in time order, so once a sample has every horizon
        // resolved it never needs revisiting. `first_open_` walks forward.
        for (size_t i = first_open_; i < samples_.size(); ++i) {
            Sample& s = samples_[i];
            bool all_done = true;
            for (size_t h = 0; h < kNumHorizons; ++h) {
                if (s.resolved[h]) continue;
                if (now_ns >= s.ts + kHorizons[h]) {
                    s.two_mid_at[h] = mid.two_mid;
                    s.resolved[h] = true;
                } else {
                    all_done = false;
                }
            }
            if (all_done && i == first_open_) ++first_open_;
        }
    }

    HorizonReport report(size_t horizon_index) const {
        HorizonReport r;
        r.horizon_ns = kHorizons[horizon_index];
        int64_t two_drift_shares = 0;
        int64_t two_edge_shares = 0;
        for (const Sample& s : samples_) {
            const int64_t sgn = s.side == Side::Buy ? 1 : -1;
            if (!s.resolved[horizon_index]) {
                ++r.unresolved_fills;
                continue;
            }
            ++r.resolved_fills;
            r.shares += s.shares;
            const int64_t q = static_cast<int64_t>(s.shares);
            two_edge_shares += sgn * (s.two_mid_at_fill - s.two_fill) * q;
            two_drift_shares += sgn * (s.two_mid_at[horizon_index] - s.two_mid_at_fill) * q;
        }
        if (r.shares > 0) {
            const Money scale = kMicrosPerPrice4 / 2;
            const auto shares = static_cast<Money>(r.shares);
            r.edge_per_share = divide_round(two_edge_shares * scale, shares);
            r.drift_per_share = divide_round(two_drift_shares * scale, shares);
            r.markout_per_share = r.edge_per_share + r.drift_per_share;
        }
        return r;
    }

    const std::vector<Sample>& samples() const { return samples_; }
    uint64_t fills_without_mid() const { return fills_without_mid_; }

private:
    std::vector<Sample> samples_;
    size_t first_open_ = 0;
    uint64_t fills_without_mid_ = 0;
};

}  // namespace itchbook::sim
