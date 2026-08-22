#pragma once
//
// intensity.hpp — measuring lambda(delta) instead of assuming it.
//
// Avellaneda-Stoikov needs a fill-intensity curve, lambda(delta) = A*exp(-k*delta),
// where delta is how far a resting order sits from the mid. Every implementation
// needs A and k. Almost every implementation assumes them -- and phase 11.1
// showed what that costs, where the paper's own k = 1.5 per dollar produced a
// 170-tick spread on a penny-wide book.
//
// This measures them, which is possible here only because the queue model knows
// two things a naive backtest does not: which of our orders actually filled, and
// how long each one was genuinely exposed at each depth.
//
// THE ESTIMATOR IS FILLS PER ORDER-SECOND, AND THE DENOMINATOR IS THE HARD PART.
//
//     lambda_hat(delta) = fills(delta) / exposure(delta)
//
// Exposure is integrated per ORDER over time: two orders resting one second at
// the same depth is two order-seconds. That is the right normalisation because
// lambda is the intensity for a SINGLE order -- with n orders resting, expected
// fills are n*lambda*dt, so dividing by order-seconds recovers lambda rather
// than something proportional to how much we happened to be quoting.
//
// Four things are excluded from the denominator, and each one would bias k if it
// were not:
//
//   * Time when the symbol is NOT TRADABLE. An order resting through a halt is
//     not being offered a fill. Counting that time inflates exposure and drags
//     the estimated intensity down at every depth.
//   * Time when the mid is UNUSABLE. Depth from a mid that does not exist is not
//     a depth.
//   * HIDDEN size. Iceberg reserve is in no queue and cannot be hit, so only the
//     displayed slice is exposed.
//   * Orders that are not live.
//
// DEPTH IS INTEGRATED, NOT ASSIGNED AT PLACEMENT. The mid moves while an order
// rests, so a single order migrates between depth buckets during its life. An
// estimator that bucketed each order by its depth at arrival would attribute all
// of its exposure -- and any fill -- to a depth it may have left seconds
// earlier. That is why this samples over time rather than counting placements.
//
// AND THE MEASUREMENT IS CONDITIONAL ON A QUEUE MODEL, which is the subtlety
// phase 11.2 says must not be left implicit. `fills(delta)` comes from whichever
// fill model produced them, so A and k are properties of (this feed, this
// strategy, THAT MODEL). The model is recorded in the output for that reason,
// and the project's decision about what to do with it is written down in
// docs/build-plan-9-12.md rather than left to whoever reads the numbers.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "itchbook/sim/event.hpp"
#include "itchbook/sim/mid.hpp"
#include "itchbook/sim/queue_model.hpp"

namespace itchbook::sim {

struct IntensityConfig {
    int32_t tick = 100;
    // Depths beyond this are pooled into an overflow bucket rather than dropped:
    // a quote 40 ticks out is real information about how little happens there,
    // and silently discarding it would bias the fit toward the touch.
    int32_t max_depth_ticks = 24;
};

struct DepthBucket {
    int32_t depth_ticks = 0;
    double exposure_seconds = 0.0;   // ORDER-seconds, not wall seconds
    uint64_t fills = 0;
    uint64_t shares = 0;

    double lambda() const {
        return exposure_seconds > 0.0 ? static_cast<double>(fills) / exposure_seconds : 0.0;
    }
};

// A log-linear fit of ln(lambda) against delta, weighted by fill count.
//
// Weighting is Poisson: a bucket's fill count is a count, so the variance of
// ln(lambda_hat) is about 1/fills, and the natural weight is fills. Unweighted
// least squares would let a bucket holding three fills pull the slope as hard as
// one holding three thousand -- and the sparse buckets are always the deep ones,
// so the bias has a direction.
struct IntensityFit {
    double A = 0.0;           // fills per order-second at delta = 0
    double k = 0.0;           // per DOLLAR of depth, matching AsConfig::k
    double r_squared = 0.0;
    size_t points = 0;        // buckets that could be fitted
    size_t dropped_no_fills = 0;   // buckets with exposure but zero fills
    // Residual degrees of freedom: points - 2, because a line has two
    // parameters. This is reported because R^2 does not carry it, and a
    // two-point "fit" of a two-parameter model returns R^2 = 1.0000 exactly --
    // not a perfect description of the data, an interpolation through it with
    // no residual left over to describe. The first real calibration produced
    // exactly that on AMD, in three of four lanes, and it read as the best fit
    // in the table.
    size_t dof = 0;
    bool ok = false;

    // The same fit with the TOUCH BUCKET EXCLUDED, which is the only way the
    // section 6.2 misfit claim can be tested at all.
    //
    // lambda-hat is Poisson-weighted, so the weight of a bucket is its fill
    // count -- and the touch bucket holds most of the fills on every symbol
    // measured (55% on GOOG, 83% on MSFT, 98% on AMD). It therefore DRAGS THE
    // LINE ONTO ITSELF, and its own residual comes back near zero by
    // construction. Asking whether the touch sits off a curve it determined is
    // circular. Fitting the deep buckets alone and then asking where the touch
    // lands is not.
    bool touch_excluded_ok = false;
    double k_ex_touch = 0.0;
    double A_ex_touch = 0.0;
    size_t points_ex_touch = 0;
    double touch_residual_ex = 0.0;   // observed ln lambda at delta=0, minus predicted
};

// The residual at each fitted point, which is where the touch misfit shows up.
struct FitResidual {
    int32_t depth_ticks = 0;
    double observed_ln_lambda = 0.0;
    double fitted_ln_lambda = 0.0;
    double residual = 0.0;
    uint64_t fills = 0;
    double exposure_seconds = 0.0;
};

class IntensityRecorder {
public:
    explicit IntensityRecorder(IntensityConfig cfg = {})
        : cfg_(cfg),
          buckets_(static_cast<size_t>(cfg.max_depth_ticks) + 2) {
        for (size_t i = 0; i < buckets_.size(); ++i) {
            buckets_[i].depth_ticks = static_cast<int32_t>(i);
        }
    }

    // Called once per message, after the book has been updated and before the
    // strategy acts. `entries` is the queue model's live set.
    void observe(uint64_t ts, const Mid& mid, const std::vector<Entry>& entries,
                 bool tradable) {
        if (!started_) {
            started_ = true;
            last_ts_ = ts;
            return;
        }
        const uint64_t dt = ts > last_ts_ ? ts - last_ts_ : 0;
        last_ts_ = ts;
        if (dt == 0) return;
        if (!tradable || !mid.ok()) {
            untradable_ns_ += dt;
            return;
        }
        const double seconds = static_cast<double>(dt) / 1e9;
        const int32_t m = static_cast<int32_t>(mid.two_mid / 2);
        for (const Entry& e : entries) {
            if (!e.live || e.display == 0) continue;
            buckets_[index_for(depth_ticks(e.side, e.price, m))].exposure_seconds += seconds;
            ++order_samples_;
        }
    }

    // A fill of ours, bucketed at the depth it had when it happened.
    void on_fill(const SimFill& f, const Mid& mid) {
        if (!mid.ok()) {
            ++fills_without_mid_;
            return;
        }
        const int32_t m = static_cast<int32_t>(mid.two_mid / 2);
        DepthBucket& b = buckets_[index_for(depth_ticks(f.side, f.price, m))];
        ++b.fills;
        b.shares += f.shares;
    }

    const std::vector<DepthBucket>& buckets() const { return buckets_; }
    uint64_t untradable_ns() const { return untradable_ns_; }
    uint64_t order_samples() const { return order_samples_; }
    uint64_t fills_without_mid() const { return fills_without_mid_; }
    // How often a resting order was found on the wrong side of the mid -- a bid
    // above it or an ask below it. Not an error: the mid moves between our
    // decision and the next message. Counted because a large number means the
    // depth-zero bucket is really a mixture and the touch misfit below has a
    // second cause.
    uint64_t crossed_observations() const { return crossed_; }
    const IntensityConfig& config() const { return cfg_; }

private:
    size_t index_for(int32_t depth) const {
        if (depth < 0) return 0;
        if (depth > cfg_.max_depth_ticks) return static_cast<size_t>(cfg_.max_depth_ticks) + 1;
        return static_cast<size_t>(depth);
    }

    int32_t depth_ticks(Side side, Price4 price, int32_t mid_price) {
        const int32_t away = (side == Side::Buy) ? (mid_price - price) : (price - mid_price);
        if (away < 0) ++crossed_;
        return away / cfg_.tick;
    }

    IntensityConfig cfg_;
    std::vector<DepthBucket> buckets_;
    uint64_t last_ts_ = 0;
    uint64_t untradable_ns_ = 0;
    uint64_t order_samples_ = 0;
    uint64_t fills_without_mid_ = 0;
    uint64_t crossed_ = 0;
    bool started_ = false;
};

// Fit ln(lambda) = ln(A) - k*delta by weighted least squares.
//
// Depth enters in DOLLARS, not ticks, so k comes out in the units AsConfig::k
// wants and the two can be compared without a conversion nobody remembers.
//
// Buckets with exposure but no fills cannot be logged and are excluded -- and
// COUNTED, because dropping them silently biases the fit upward: the deep
// buckets are exactly the ones that fail to fill, so discarding them quietly
// flattens the curve.
inline IntensityFit fit_intensity(const std::vector<DepthBucket>& buckets,
                                  int32_t tick,
                                  std::vector<FitResidual>* residuals = nullptr) {
    IntensityFit out;
    double sw = 0.0, swx = 0.0, swy = 0.0, swxx = 0.0, swxy = 0.0;
    struct Point {
        double x, y, w;
        int32_t depth;
        uint64_t fills;
        double exposure;
    };
    std::vector<Point> pts;
    for (const DepthBucket& b : buckets) {
        if (b.exposure_seconds <= 0.0) continue;
        if (b.fills == 0) {
            ++out.dropped_no_fills;
            continue;
        }
        const double x = static_cast<double>(b.depth_ticks) *
                         static_cast<double>(tick) / 10000.0;
        const double y = std::log(b.lambda());
        const double w = static_cast<double>(b.fills);
        pts.push_back(Point{x, y, w, b.depth_ticks, b.fills, b.exposure_seconds});
        sw += w;
        swx += w * x;
        swy += w * y;
        swxx += w * x * x;
        swxy += w * x * y;
    }
    out.points = pts.size();
    out.dof = pts.size() >= 2 ? pts.size() - 2 : 0;

    // THREE POINTS, NOT TWO. A line has two parameters, so two points determine
    // it exactly: the residuals are zero, R^2 comes out 1.0000, and the number
    // that looks like the strongest fit in the table is the one with no
    // evidence behind it. Requiring dof >= 1 means every fit reported here has
    // at least one observation the model had to survive rather than absorb.
    //
    // This is not a formality on this data. On a symbol whose spread is pinned
    // at one tick, EVERY fill lands at depth 0 or 1 -- delta is measured from
    // the mid, so the identifiable range of delta IS the half-spread. A-S's
    // fill-intensity curve is therefore not estimable on a tick-constrained
    // name, and saying so is a result. Silently interpolating two points and
    // calling it k is not.
    if (pts.size() < 3) return out;
    const double denom = sw * swxx - swx * swx;
    if (std::fabs(denom) < 1e-30) return out;
    const double slope = (sw * swxy - swx * swy) / denom;
    const double intercept = (swy - slope * swx) / sw;
    out.k = -slope;                       // lambda decays, so the slope is negative
    out.A = std::exp(intercept);
    out.ok = true;

    const double ybar = swy / sw;
    double ss_res = 0.0, ss_tot = 0.0;
    for (const Point& p : pts) {
        const double fitted = intercept + slope * p.x;
        ss_res += p.w * (p.y - fitted) * (p.y - fitted);
        ss_tot += p.w * (p.y - ybar) * (p.y - ybar);
        if (residuals != nullptr) {
            residuals->push_back(FitResidual{p.depth, p.y, fitted, p.y - fitted,
                                             p.fills, p.exposure});
        }
    }
    out.r_squared = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 0.0;

    // ---- the same fit without the touch, so section 6.2 becomes testable ----
    double ew = 0.0, ewx = 0.0, ewy = 0.0, ewxx = 0.0, ewxy = 0.0;
    size_t en = 0;
    const Point* touch = nullptr;
    for (const Point& p : pts) {
        if (p.depth == 0) { touch = &p; continue; }
        ++en;
        ew += p.w; ewx += p.w * p.x; ewy += p.w * p.y;
        ewxx += p.w * p.x * p.x; ewxy += p.w * p.x * p.y;
    }
    out.points_ex_touch = en;
    if (en >= 3) {
        const double d2 = ew * ewxx - ewx * ewx;
        if (std::fabs(d2) >= 1e-30) {
            const double slope2 = (ew * ewxy - ewx * ewy) / d2;
            const double inter2 = (ewy - slope2 * ewx) / ew;
            out.k_ex_touch = -slope2;
            out.A_ex_touch = std::exp(inter2);
            // A NEGATIVE k IS NOT A FILL CURVE. lambda = A*exp(-k*delta) with
            // k < 0 says orders resting FURTHER from the mid fill FASTER, which
            // is not a shape any book produces; it is what a regression returns
            // when the deep buckets are three single-fill points with almost no
            // exposure between them. Seen on the thin name at the first real
            // calibration, in all four lanes. Reporting the number would put a
            // fill curve in the paper that runs backwards, so the fit is marked
            // unusable and the touch is left unjudged rather than judged against
            // nonsense.
            out.touch_excluded_ok = out.k_ex_touch > 0.0;
            if (out.touch_excluded_ok && touch != nullptr) {
                out.touch_residual_ex = touch->y - (inter2 + slope2 * touch->x);
            }
        }
    }
    return out;
}

}  // namespace itchbook::sim
