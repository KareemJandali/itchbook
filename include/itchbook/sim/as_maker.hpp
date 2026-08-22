#pragma once
//
// as_maker.hpp — Avellaneda-Stoikov (2008), finite horizon, on a real book.
//
// Two quotes around a RESERVATION PRICE that is not the mid. The whole model is
// in that displacement:
//
//     r(s, q, t) = s - q * gamma * sigma^2 * (T - t)
//
// Long inventory pushes r below the mid, so both quotes move down, so the ask
// is more likely to trade and the bid less. The strategy is not predicting the
// price; it is pricing its own risk of being caught holding inventory when the
// session ends. The optimal total spread is
//
//     delta_a + delta_b = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
//
// split symmetrically about r. The first term is inventory risk, the second is
// what the fill intensity lets you charge.
//
// UNITS, BECAUSE THIS IS WHERE THE MODEL IS USUALLY GOT QUIETLY WRONG.
//
// The paper's two terms are not dimensionally consistent with each other unless
// inventory is treated as a dimensionless count. In the spread term, gamma
// multiplies sigma^2*(T-t), which is a price squared, and the result must be a
// price -- so gamma is 1/price. In the reservation term, the same gamma
// multiplies q*sigma^2*(T-t), which is shares times price squared, and the
// result must also be a price -- so gamma would have to be 1/(shares*price).
// Both cannot hold.
//
// Implementations resolve this silently and differently, which is one reason
// published A-S results are hard to compare. This one resolves it explicitly:
//
//   * everything is computed in DOLLARS and SECONDS, never in Price4 ticks or
//     nanoseconds, because a variance in units of 1e-8 dollars squared per
//     nanosecond is a number nobody can sanity-check;
//   * gamma has units 1/dollar in BOTH terms;
//   * inventory enters as q / inventory_unit, a dimensionless multiple of a
//     stated share count. That parameter is the modelling choice the paper
//     leaves implicit, and it is named so it can be swept and reported.
//
// Set inventory_unit to the quote size and "q = 1" means "one clip long".
//
// THE END-OF-HORIZON PATHOLOGY, FLAGGED RATHER THAN HIDDEN.
//
// As t approaches T, (T - t) goes to zero, so the inventory term vanishes and
// the spread collapses to the intensity term alone. The strategy stops caring
// about inventory exactly when it has the least time left to unload it. That is
// not a bug in this implementation, it is a property of the finite-horizon
// model: it assumes terminal inventory is liquidated at the mid, which a real
// desk cannot do.
//
// Two things follow. `min_time_remaining` can floor (T - t) as a mitigation, and
// it DEFAULTS TO ZERO so the pathology is visible rather than papered over --
// a run that wants the mitigation has to ask. And the strategy counts how many
// quotes it placed in the last tenth of the session while holding inventory, so
// the pathology shows up as a number in the results rather than as a paragraph
// nobody reads. Gueant-Lehalle-Fernandez-Tapia's inventory-bounded variant is
// the principled fix and is not implemented here.
//
// SIGMA IS ESTIMATED ONLINE, from the realised variance of mid CHANGES rather
// than of returns. A-S is a model of arithmetic Brownian motion: the volatility
// it wants is in dollars per root-second, not in percent. Sampling is on a fixed
// time grid rather than per message, because per-message sampling makes the
// estimate a function of how busy the symbol is.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "itchbook/sim/inventory_strategy.hpp"
#include "itchbook/sim/money.hpp"
#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

// Rolling realised variance of the mid, on a fixed time grid.
//
// A ring of the last N sampled mids; the estimate is the variance of successive
// differences, scaled to per-second. Allocation-free after construction.
class MidVolatility {
public:
    explicit MidVolatility(size_t window = 120, uint64_t sample_ns = 1000000000ULL)
        : samples_(window == 0 ? 1 : window), sample_ns_(sample_ns == 0 ? 1 : sample_ns) {}

    // Feed it every usable mid; it decides when to take a sample.
    void observe(uint64_t ts, double mid_dollars) {
        if (!started_) {
            started_ = true;
            next_sample_ = ts;
        }
        if (ts < next_sample_) return;
        // Snap forward rather than adding one interval, so a gap in the feed
        // does not produce a burst of catch-up samples all holding the same
        // price and collapsing the variance to zero.
        while (next_sample_ <= ts) next_sample_ += sample_ns_;
        if (count_ > 0) {
            const double d = mid_dollars - last_;
            diffs_[write_ % diffs_.size()] = d;
            ++diff_count_;
        }
        last_ = mid_dollars;
        samples_[write_ % samples_.size()] = mid_dollars;
        ++write_;
        ++count_;
    }

    // Variance of mid changes per SECOND, in dollars squared.
    //
    // Returns a floor rather than zero when there is not enough history: a
    // sigma of zero makes the spread term collapse and the strategy quote at
    // the reservation price on both sides, which is not conservative, it is
    // nonsense.
    double variance_per_second(double floor = 1e-8) const {
        const size_t n = std::min<size_t>(diff_count_, diffs_.size());
        if (n < 2) return floor;
        double mean = 0.0;
        for (size_t i = 0; i < n; ++i) mean += diffs_[i];
        mean /= static_cast<double>(n);
        double ss = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double d = diffs_[i] - mean;
            ss += d * d;
        }
        const double per_sample = ss / static_cast<double>(n - 1);
        const double seconds = static_cast<double>(sample_ns_) / 1e9;
        const double v = per_sample / seconds;
        return v > floor ? v : floor;
    }

    uint64_t samples_taken() const { return count_; }

    // State, not configuration: the window length and the sampling interval are
    // instructions and are deliberately absent.
    struct State {
        std::vector<double> samples;
        std::vector<double> diffs;
        uint64_t write = 0;
        uint64_t count = 0;
        uint64_t diff_count = 0;
        uint64_t next_sample = 0;
        double last = 0.0;
        bool started = false;
    };
    State state() const {
        return State{samples_, diffs_, write_, count_, diff_count_, next_sample_,
                     last_, started_};
    }
    void restore(const State& s) {
        // A restored ring must not resize the configured window: the snapshot
        // carries what was learned, the constructor carries what was asked for.
        // Copying only what fits keeps a stale snapshot from silently changing
        // the estimator's shape.
        const size_t n = std::min(s.samples.size(), samples_.size());
        for (size_t i = 0; i < n; ++i) samples_[i] = s.samples[i];
        const size_t m = std::min(s.diffs.size(), diffs_.size());
        for (size_t i = 0; i < m; ++i) diffs_[i] = s.diffs[i];
        write_ = s.write;
        count_ = s.count;
        diff_count_ = s.diff_count;
        next_sample_ = s.next_sample;
        last_ = s.last;
        started_ = s.started;
    }

private:
    std::vector<double> samples_;
    std::vector<double> diffs_ = std::vector<double>(samples_.size(), 0.0);
    uint64_t sample_ns_;
    uint64_t write_ = 0;
    uint64_t count_ = 0;
    uint64_t diff_count_ = 0;
    uint64_t next_sample_ = 0;
    double last_ = 0.0;
    bool started_ = false;
};

// THE DEFAULTS ARE IN EQUITY UNITS, AND THE PAPER'S ARE NOT.
//
// Avellaneda-Stoikov's worked example uses s = 100, sigma = 2, gamma = 0.1,
// k = 1.5 in arbitrary units, and those numbers get copied into implementations
// unchanged. Applied to a penny-spread equity they are catastrophic. The
// intensity term is (2/gamma)*ln(1 + gamma/k), which for small gamma/k is
// approximately 2/k -- so k = 1.5 per dollar means a $1.33 half spread, a
// TOTAL SPREAD OF ABOUT 129 TICKS on a book whose real spread is one or two.
// The strategy quotes all day and never fills. That is not a subtle bias; it is
// the first thing that happened when this was run, and the tests below now
// refuse to let it happen quietly.
//
// What k means in equity units: lambda(delta) = A*exp(-k*delta) with delta in
// dollars, so 1/k is the depth over which fill intensity falls by a factor of
// e. For a liquid US equity that is a fraction of a cent to a few cents, which
// puts k in the hundreds, not near one. k = 200 gives an intensity term of
// about 2/200 = $0.01, one tick, which is the right order for a name that
// quotes a penny wide.
//
// gamma follows from the inventory term. A $100 name with 2% daily vol has
// sigma^2 of roughly 1.7e-4 dollars^2 per second, and sigma^2 * tau reaches
// about 4 dollars^2 at the open. gamma = 0.005 then skews the quote by about
// two cents per unit of inventory, which is a real but not overwhelming
// displacement.
//
// Both remain placeholders in the sense that matters: gamma is SWEPT rather
// than chosen, and k is MEASURED in phase 11.2 from our own fills, which is the
// centrepiece of this phase precisely because everyone else assumes it.
struct AsConfig {
    // Risk aversion, 1/dollar. Swept, never chosen.
    double gamma = 0.005;

    // Fill-intensity decay, per dollar of depth. Replaced by the phase 11.2
    // measurement; until then this is an equity-plausible placeholder rather
    // than the paper's simulation value.
    double k = 200.0;

    // Inventory unit, in shares. q enters the model as position/inventory_unit,
    // which is the dimensionless count the paper's algebra assumes. Set it to
    // the quote size and q = 1 means one clip long.
    double inventory_unit = 100.0;

    uint32_t quote_size = 100;
    int32_t tick = 100;                 // Price4 units: one cent

    // How far from the mid a quote may sit, in ticks. A-S will happily quote
    // hundreds of ticks wide when inventory is large and the horizon long;
    // those quotes never fill and clutter the book, and a real desk would not
    // send them.
    int32_t max_ticks_from_mid = 200;

    // Re-quote when the reservation price has moved this many ticks since the
    // last quote, or on a fill. NOT on every message: a strategy that re-quotes
    // on every tick is a strategy whose message rate is its defining feature,
    // and re-quotes are counted here because they are a cost.
    int32_t requote_ticks = 1;

    // Floor on (T - t), in seconds. Zero is pure Avellaneda-Stoikov, including
    // its end-of-horizon pathology. Non-zero is a mitigation and has to be
    // asked for.
    double min_time_remaining = 0.0;

    // The session horizon in seconds, used to turn the harness's [0,1] progress
    // back into (T - t). The regular US equity session is 6.5 hours.
    double horizon_seconds = 6.5 * 3600.0;

    size_t vol_window = 120;
    uint64_t vol_sample_ns = 1000000000ULL;
};

// The model itself, as a pure function of its inputs.
//
// Separated from the strategy deliberately. Everything above this line is
// Avellaneda-Stoikov; everything below it is plumbing -- books, ticks, quote
// ids, re-quote policy. Keeping the two apart means the algebra can be checked
// against the paper directly, with no book to build and no feed to replay, and
// a test that disagrees with it is disagreeing about the model rather than
// about the harness.
struct AsQuote {
    double reservation = 0.0;    // dollars
    double half_spread = 0.0;    // dollars
    double bid() const { return reservation - half_spread; }
    double ask() const { return reservation + half_spread; }
};

// tau is (T - t) in seconds, sigma2 the variance of mid changes per second in
// dollars squared, q_units the dimensionless inventory (position divided by
// inventory_unit; see the units note at the top of this file).
inline AsQuote as_quote(const AsConfig& cfg, double mid, double q_units, double tau,
                        double sigma2) {
    AsQuote out;
    out.reservation = mid - q_units * cfg.gamma * sigma2 * tau;
    // gamma = 0 is not a degenerate input, it is the CONTROL: no inventory
    // skew, and a spread set purely by the fill intensity. It is how the
    // experiment isolates inventory-awareness from the choice of spread, which
    // a touch-maker comparison confounds.
    //
    // The formula is 0/0 there and the limit is finite: as gamma -> 0,
    // (2/gamma)*ln(1 + gamma/k) -> (2/gamma)*(gamma/k) = 2/k. Taking the limit
    // rather than refusing the input keeps the control inside the same code
    // path as the treatment, which is the only way the two are comparable.
    const double intensity = cfg.gamma > 1e-12
                                 ? (2.0 / cfg.gamma) * std::log1p(cfg.gamma / cfg.k)
                                 : 2.0 / cfg.k;
    out.half_spread = (cfg.gamma * sigma2 * tau + intensity) / 2.0;
    return out;
}

struct AsReport {
    uint64_t quotes = 0;
    uint64_t requotes = 0;
    uint64_t cancels = 0;
    uint64_t clamped_by_max_distance = 0;
    uint64_t skipped_no_mid = 0;
    // The pathology, as a number: quotes placed in the last tenth of the session
    // while holding inventory, when the model's own inventory term has decayed
    // to almost nothing.
    uint64_t late_session_quotes_with_inventory = 0;
    int64_t max_abs_position = 0;
    double last_reservation = 0.0;
    double last_half_spread = 0.0;
    double last_sigma2 = 0.0;
};

class AsMaker {
public:
    explicit AsMaker(AsConfig cfg = {})
        : cfg_(cfg), vol_(cfg.vol_window, cfg.vol_sample_ns) {}

    void on_event(const InventoryView& v, Ctx& ctx) {
        int32_t bid = 0;
        int32_t ask = 0;
        if (!v.tradable || !v.mid.ok() || !v.best_bid(&bid) || !v.best_ask(&ask)) {
            ++report_.skipped_no_mid;
            return;
        }
        const double mid = static_cast<double>(v.mid.two_mid) / 2.0 / 10000.0;
        vol_.observe(v.ts, mid);

        const double sigma2 = vol_.variance_per_second();
        double tau = (1.0 - v.session_elapsed) * cfg_.horizon_seconds;
        if (tau < cfg_.min_time_remaining) tau = cfg_.min_time_remaining;

        const double q = static_cast<double>(v.position) / cfg_.inventory_unit;
        const AsQuote model = as_quote(cfg_, mid, q, tau, sigma2);
        const double reservation = model.reservation;
        const double half = model.half_spread;

        report_.last_reservation = reservation;
        report_.last_half_spread = half;
        report_.last_sigma2 = sigma2;
        const int64_t abs_pos = v.position < 0 ? -v.position : v.position;
        if (abs_pos > report_.max_abs_position) report_.max_abs_position = abs_pos;

        // Re-quote only when the reservation price has moved enough, or when a
        // fill has changed our inventory since the last quote.
        const double moved = std::fabs(reservation - last_reservation_);
        const double threshold =
            static_cast<double>(cfg_.requote_ticks) * static_cast<double>(cfg_.tick) / 10000.0;
        if (quoted_ && !fill_since_quote_ && moved < threshold) return;

        const int32_t want_bid = clamp_to_book(to_ticks(reservation - half, /*down=*/true),
                                               v.mid, /*is_bid=*/true);
        const int32_t want_ask = clamp_to_book(to_ticks(reservation + half, /*down=*/false),
                                               v.mid, /*is_bid=*/false);
        // A crossed or locked pair of our own quotes is not a quote, it is two
        // takes. The model can produce one when inventory is large and the
        // spread term small; refusing is the honest response.
        if (want_bid >= want_ask) return;

        if (quoted_) {
            ctx.cancel(bid_id_);
            ctx.cancel(ask_id_);
            report_.cancels += 2;
            ++report_.requotes;
        }
        bid_id_ = next_id_++;
        ask_id_ = next_id_++;
        ctx.quote(bid_id_, Side::Buy, want_bid, cfg_.quote_size);
        ctx.quote(ask_id_, Side::Sell, want_ask, cfg_.quote_size);
        report_.quotes += 2;
        if (v.session_elapsed > 0.9 && v.position != 0) {
            report_.late_session_quotes_with_inventory += 2;
        }
        quoted_ = true;
        fill_since_quote_ = false;
        last_reservation_ = reservation;
    }

    void on_fill(const FillEvent&) { fill_since_quote_ = true; }

    const AsReport& report() const { return report_; }
    const AsConfig& config() const { return cfg_; }

    // State/restore. Configuration -- gamma, k, the window, the clamps -- is
    // deliberately absent: it is the operator's instruction, and a snapshot that
    // restored it would let a stale run silently override a corrected one. What
    // travels is what was learned: the volatility estimator, the live quote
    // ids, and the counters.
    struct State {
        MidVolatility::State vol;
        uint64_t next_id = 1;
        uint64_t bid_id = 0;
        uint64_t ask_id = 0;
        double last_reservation = 0.0;
        bool quoted = false;
        bool fill_since_quote = false;
        AsReport report;
    };
    State state() const {
        return State{vol_.state(),   next_id_, bid_id_,          ask_id_,
                     last_reservation_, quoted_, fill_since_quote_, report_};
    }
    void restore(const State& s) {
        vol_.restore(s.vol);
        next_id_ = s.next_id;
        bid_id_ = s.bid_id;
        ask_id_ = s.ask_id;
        last_reservation_ = s.last_reservation;
        quoted_ = s.quoted;
        fill_since_quote_ = s.fill_since_quote;
        report_ = s.report;
    }

private:
    int32_t to_ticks(double dollars, bool down) const {
        const double raw = dollars * 10000.0;
        const double t = static_cast<double>(cfg_.tick);
        // A bid rounds DOWN and an ask rounds UP, so rounding never tightens a
        // quote into a price the model did not ask for.
        const double snapped = down ? std::floor(raw / t) * t : std::ceil(raw / t) * t;
        return static_cast<int32_t>(snapped);
    }

    int32_t clamp_to_book(int32_t price, const Mid& mid, bool is_bid) {
        const int32_t m = static_cast<int32_t>(mid.two_mid / 2);
        const int32_t limit = cfg_.max_ticks_from_mid * cfg_.tick;
        int32_t out = price;
        if (is_bid && price < m - limit) out = m - limit;
        if (!is_bid && price > m + limit) out = m + limit;
        if (out != price) ++report_.clamped_by_max_distance;
        if (out < cfg_.tick) out = cfg_.tick;      // never quote at or below zero
        return out;
    }

    AsConfig cfg_;
    MidVolatility vol_;
    AsReport report_;
    uint64_t next_id_ = 1;
    uint64_t bid_id_ = 0;
    uint64_t ask_id_ = 0;
    double last_reservation_ = 0.0;
    bool quoted_ = false;
    bool fill_since_quote_ = false;
};

}  // namespace itchbook::sim
