// Measuring lambda(delta) instead of assuming it.
//
// Two things are under test and they fail differently. The FIT is arithmetic
// with a closed form, so it is checked by generating data from a known A and k
// and requiring them back. The RECORDER is about what counts as exposure, so it
// is checked one exclusion at a time -- each of the four things it refuses to
// count would bias k in a stated direction if it were counted.
#include <cmath>
#include <cstdint>
#include <vector>

#include "itchbook/sim/as_maker.hpp"
#include "itchbook/sim/closed_loop.hpp"
#include "itchbook/sim/intensity.hpp"
#include "tests/check.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

bool close_to(double a, double b, double tol) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}

std::vector<DepthBucket> synth(double A, double k, int32_t tick, int n,
                               double exposure = 1000.0) {
    std::vector<DepthBucket> out;
    for (int d = 0; d < n; ++d) {
        const double delta = static_cast<double>(d) * static_cast<double>(tick) / 10000.0;
        const double lambda = A * std::exp(-k * delta);
        DepthBucket b;
        b.depth_ticks = d;
        b.exposure_seconds = exposure;
        // Rounded, because fills are a count. The rounding is the only reason
        // the recovery below is not exact.
        b.fills = static_cast<uint64_t>(lambda * exposure + 0.5);
        b.shares = b.fills * 100;
        out.push_back(b);
    }
    return out;
}

// ---- the fit --------------------------------------------------------------

void test_it_recovers_a_known_intensity_curve() {
    const double A = 0.8, k = 150.0;
    const IntensityFit f = fit_intensity(synth(A, k, 100, 12), 100);
    CHECK(f.ok);
    CHECK(close_to(f.k, k, 0.02));
    CHECK(close_to(f.A, A, 0.05));
    CHECK(f.r_squared > 0.99);
}

// k comes out per DOLLAR, matching AsConfig::k, so the two can be compared
// without a conversion nobody remembers. A curve generated in ticks must fit to
// the same k whatever the tick size.
void test_k_is_per_dollar_not_per_tick() {
    const double k = 200.0;
    const IntensityFit penny = fit_intensity(synth(1.0, k, 100, 14), 100);
    const IntensityFit half = fit_intensity(synth(1.0, k, 50, 28), 50);
    CHECK(close_to(penny.k, k, 0.05));
    CHECK(close_to(half.k, k, 0.05));
}

// Buckets with exposure but no fills cannot be logged. Dropping them silently
// would bias the fit UPWARD, because the deep buckets are exactly the ones that
// fail to fill -- so they are counted and reported.
void test_empty_buckets_are_dropped_and_counted() {
    std::vector<DepthBucket> b = synth(0.5, 300.0, 100, 20);
    size_t empties = 0;
    for (const DepthBucket& x : b) {
        if (x.fills == 0 && x.exposure_seconds > 0) ++empties;
    }
    CHECK(empties > 0);        // the generator really did produce some
    const IntensityFit f = fit_intensity(b, 100);
    CHECK_EQ(f.dropped_no_fills, empties);
    CHECK_EQ(f.points, b.size() - empties);
}

// Weighting is Poisson: a bucket holding three fills must not pull the slope as
// hard as one holding three thousand. Unweighted least squares would let the
// sparse deep buckets dominate, and the bias has a direction.
void test_the_fit_is_weighted_by_fill_count() {
    std::vector<DepthBucket> b = synth(1.0, 150.0, 100, 8);
    const IntensityFit clean = fit_intensity(b, 100);
    // Corrupt the sparsest bucket badly. A weighted fit barely notices.
    DepthBucket& deep = b.back();
    CHECK(deep.fills < b.front().fills / 10);
    deep.fills = std::max<uint64_t>(1, deep.fills * 8);
    const IntensityFit perturbed = fit_intensity(b, 100);
    CHECK(close_to(perturbed.k, clean.k, 0.35));
}

void test_a_fit_needs_at_least_two_points() {
    std::vector<DepthBucket> one;
    DepthBucket b;
    b.depth_ticks = 0;
    b.exposure_seconds = 10.0;
    b.fills = 5;
    one.push_back(b);
    CHECK(!fit_intensity(one, 100).ok);
    CHECK(!fit_intensity({}, 100).ok);
}

// TWO POINTS IS NOT A FIT, and the number it produces is the most flattering
// one in the table. A line has two parameters, so two points determine it
// exactly: the residuals are zero and R^2 comes back 1.0000. The first real
// calibration did exactly this on AMD in three of four lanes -- the symbol
// whose spread is pinned at one tick, so every fill lands at depth 0 or 1 and
// there is no third depth to fit.
void test_two_points_are_refused_because_they_cannot_fail() {
    std::vector<DepthBucket> two = synth(1.0, 150.0, 100, 2);
    const IntensityFit f = fit_intensity(two, 100);
    CHECK(!f.ok);
    CHECK_EQ(f.points, 2u);
    CHECK_EQ(f.dof, 0u);

    // Three points fit, and carry one degree of freedom the model had to
    // survive rather than absorb.
    const IntensityFit g = fit_intensity(synth(1.0, 150.0, 100, 3), 100);
    CHECK(g.ok);
    CHECK_EQ(g.dof, 1u);
}

// The section 6.2 touch test is circular unless the touch is left OUT of the
// fit. lambda-hat is Poisson-weighted, so the touch bucket -- which holds most
// of the fills on every symbol measured -- drags the line onto itself and its
// own residual comes back near zero whatever the truth is. Fitting the deep
// buckets alone and asking where the touch lands is the test that can fail.
void test_the_touch_is_judged_against_a_fit_it_did_not_influence() {
    std::vector<DepthBucket> b = synth(1.0, 150.0, 100, 10);
    // A touch that fills at a THIRD of the exponential's rate, and carries a
    // crushing weight while doing it: this is the shape the real data has.
    b[0].fills /= 3;
    b[0].fills *= 20;
    b[0].exposure_seconds *= 60.0;
    const IntensityFit f = fit_intensity(b, 100);
    CHECK(f.ok);
    CHECK(f.touch_excluded_ok);
    // The deep-only fit must not have been moved by the touch...
    CHECK(close_to(f.k_ex_touch, 150.0, 1.0));
    // ...and against it, the starved touch sits BELOW the curve.
    CHECK(f.touch_residual_ex < 0.0);
    // The all-points fit, which the touch dominates, hides that.
    CHECK(std::fabs(f.k_ex_touch - f.k) > 1.0);
}

// THE FINDING THE PLAN PREDICTS: the exponential fits badly at the touch,
// because at delta = 0 queue position dominates and an order at the front of a
// long queue does not fill at the rate an exponential extrapolated from deeper
// levels would suggest. The residual is the figure that shows it.
void test_the_touch_misfit_shows_up_as_a_residual() {
    std::vector<DepthBucket> b = synth(1.0, 150.0, 12, 100);
    // Queue position at the touch: the same exposure, a third of the fills.
    b[0].fills /= 3;
    std::vector<FitResidual> res;
    const IntensityFit f = fit_intensity(b, 100, &res);
    CHECK(f.ok);
    CHECK(!res.empty());
    const FitResidual* touch = nullptr;
    for (const FitResidual& r : res) {
        if (r.depth_ticks == 0) touch = &r;
    }
    CHECK(touch != nullptr);
    if (touch != nullptr) {
        // Observed intensity BELOW the fitted curve, which is what queue
        // position does and what the paper's model cannot express.
        CHECK(touch->residual < 0.0);
        CHECK(std::fabs(touch->residual) > 0.5);
    }
}

// ---- the recorder: what counts as exposure --------------------------------

std::vector<Entry> resting(std::vector<std::pair<Side, Price4>> orders,
                           uint32_t display = 100) {
    std::vector<Entry> out;
    uint64_t id = 1;
    for (auto [side, px] : orders) {
        Entry e;
        e.id = id++;
        e.side = side;
        e.price = px;
        e.display = display;
        e.live = true;
        out.push_back(e);
    }
    return out;
}

Mid mid_at(int32_t price) {
    Mid m;
    m.two_mid = static_cast<int64_t>(price) * 2;
    m.status = MidStatus::Ok;
    return m;
}

const uint64_t kSec = 1000000000ULL;

void test_exposure_is_per_order_second() {
    IntensityRecorder r;
    const auto two = resting({{Side::Buy, 999900}, {Side::Sell, 1000100}});
    r.observe(0, mid_at(1000000), two, true);
    r.observe(kSec, mid_at(1000000), two, true);
    // Both orders are one tick from the mid, so one second of wall time is TWO
    // order-seconds in bucket 1.
    CHECK(close_to(r.buckets()[1].exposure_seconds, 2.0, 1e-9));
    CHECK_EQ(r.buckets()[0].exposure_seconds, 0.0);
}

void test_untradable_time_is_excluded() {
    IntensityRecorder r;
    const auto one = resting({{Side::Buy, 999900}});
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, mid_at(1000000), one, false);           // halted
    r.observe(2 * kSec, mid_at(1000000), one, true);        // trading again
    // Only the last interval counts: an order resting through a halt is not
    // being offered a fill, and counting it drags every lambda down.
    CHECK(close_to(r.buckets()[1].exposure_seconds, 1.0, 1e-9));
    CHECK_EQ(r.untradable_ns(), kSec);
}

void test_an_unusable_mid_is_excluded() {
    IntensityRecorder r;
    const auto one = resting({{Side::Buy, 999900}});
    Mid bad;
    bad.status = MidStatus::Empty;
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, bad, one, true);
    CHECK_EQ(r.buckets()[1].exposure_seconds, 0.0);
    CHECK_EQ(r.untradable_ns(), kSec);
}

void test_hidden_size_is_not_exposed() {
    IntensityRecorder r;
    auto one = resting({{Side::Buy, 999900}});
    one[0].display = 0;         // all reserve, nothing in the queue
    one[0].hidden = 5000;
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, mid_at(1000000), one, true);
    for (const DepthBucket& b : r.buckets()) CHECK_EQ(b.exposure_seconds, 0.0);
}

void test_a_dead_entry_is_not_exposed() {
    IntensityRecorder r;
    auto one = resting({{Side::Buy, 999900}});
    one[0].live = false;
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, mid_at(1000000), one, true);
    for (const DepthBucket& b : r.buckets()) CHECK_EQ(b.exposure_seconds, 0.0);
}

// The point of integrating rather than assigning at placement: one order at a
// fixed price migrates between buckets as the mid moves.
void test_depth_is_integrated_as_the_mid_moves() {
    IntensityRecorder r;
    const auto one = resting({{Side::Buy, 999900}});
    r.observe(0, mid_at(1000000), one, true);            // 1 tick away
    r.observe(kSec, mid_at(1000000), one, true);
    r.observe(2 * kSec, mid_at(1000300), one, true);     // now 4 ticks away
    r.observe(3 * kSec, mid_at(1000300), one, true);
    CHECK(close_to(r.buckets()[1].exposure_seconds, 1.0, 1e-9));
    CHECK(close_to(r.buckets()[4].exposure_seconds, 2.0, 1e-9));
}

void test_deep_orders_pool_into_an_overflow_bucket() {
    IntensityConfig c;
    c.max_depth_ticks = 5;
    IntensityRecorder r(c);
    const auto one = resting({{Side::Buy, 990000}});     // 100 ticks away
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, mid_at(1000000), one, true);
    // Pooled, not dropped: a quote 100 ticks out is real information about how
    // little happens there.
    CHECK(close_to(r.buckets().back().exposure_seconds, 1.0, 1e-9));
}

void test_a_fill_is_bucketed_at_its_own_depth() {
    IntensityRecorder r;
    SimFill f;
    f.side = Side::Buy;
    f.price = 999700;          // 3 ticks below a 1000000 mid
    f.shares = 200;
    r.on_fill(f, mid_at(1000000));
    CHECK_EQ(r.buckets()[3].fills, uint64_t{1});
    CHECK_EQ(r.buckets()[3].shares, uint64_t{200});
    // A fill with no usable mid has no depth, so it is counted separately
    // rather than assigned to bucket zero.
    Mid bad;
    bad.status = MidStatus::Empty;
    r.on_fill(f, bad);
    CHECK_EQ(r.fills_without_mid(), uint64_t{1});
    CHECK_EQ(r.buckets()[3].fills, uint64_t{1});
}

// A bid above the mid is not an error -- the mid moves between our decision and
// the next message -- but it means bucket zero is a mixture, so it is counted.
void test_crossed_observations_are_counted_not_hidden() {
    IntensityRecorder r;
    const auto one = resting({{Side::Buy, 1000200}});    // above the mid
    r.observe(0, mid_at(1000000), one, true);
    r.observe(kSec, mid_at(1000000), one, true);
    CHECK(r.crossed_observations() > 0);
    CHECK(close_to(r.buckets()[0].exposure_seconds, 1.0, 1e-9));
}

// ---- end to end -----------------------------------------------------------

struct Feed {
    std::vector<std::vector<uint8_t>> msgs;
    uint64_t ts = 34200ULL * 1000000000ULL;
    uint64_t next_ref = 1;
    std::vector<uint8_t>& add(char t, size_t len) {
        msgs.emplace_back(len, 0);
        auto& m = msgs.back();
        m[0] = static_cast<uint8_t>(t);
        m[1] = 0; m[2] = 1;
        ts += 20000000ULL;
        for (int i = 0; i < 6; ++i)
            m[static_cast<size_t>(5 + i)] =
                static_cast<uint8_t>((ts >> (8 * (5 - i))) & 0xff);
        return m;
    }
    static void be64(std::vector<uint8_t>& m, size_t o, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            m[o + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * (7 - i))) & 0xff);
    }
    static void be32(std::vector<uint8_t>& m, size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            m[o + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * (3 - i))) & 0xff);
    }
    void sys(char c) { add('S', 12)[11] = static_cast<uint8_t>(c); }
    uint64_t order(char side, uint32_t sh, int32_t px) {
        const uint64_t ref = next_ref++;
        auto& m = add('A', 36);
        be64(m, 11, ref);
        m[19] = static_cast<uint8_t>(side);
        be32(m, 20, sh);
        for (size_t i = 0; i < 8; ++i) m[24 + i] = static_cast<uint8_t>(' ');
        m[24] = 'T'; m[25] = 'E'; m[26] = 'S'; m[27] = 'T';
        be32(m, 32, static_cast<uint32_t>(px));
        return ref;
    }
    void exec(uint64_t ref, uint32_t sh) {
        auto& m = add('E', 31);
        be64(m, 11, ref);
        be32(m, 19, sh);
        be64(m, 23, next_ref++);
    }
    void del(uint64_t ref) { be64(add('D', 19), 11, ref); }
};

void test_a_real_run_produces_exposure_and_fills() {
    Feed f;
    f.sys('O');
    f.sys('Q');
    for (int i = 0; i < 400; ++i) {
        const int32_t d = static_cast<int32_t>((i * 7) % 21 - 10) * 100;
        const uint64_t b = f.order('B', 500, 999000 + d);
        const uint64_t a = f.order('S', 500, 999200 + d);
        f.exec(b, 200);
        f.exec(a, 200);
        f.del(b);
        f.del(a);
    }
    f.sys('M');
    f.sys('C');

    AsConfig c;
    ClosedLoopBacktest<AsMaker> bt(AsMaker{c}, Model::Optimistic, FeeSchedule{});
    for (const auto& m : f.msgs) {
        bt.on_message(static_cast<char>(m[0]), m.data(), static_cast<uint16_t>(m.size()));
    }
    const IntensityRecorder& rec = bt.intensity();
    CHECK(rec.order_samples() > 0);          // orders really did rest
    double total_exposure = 0.0;
    uint64_t total_fills = 0;
    for (const DepthBucket& b : rec.buckets()) {
        total_exposure += b.exposure_seconds;
        total_fills += b.fills;
    }
    CHECK(total_exposure > 0.0);
    CHECK(total_fills > 0);
    // Every recorded fill was a maker fill with exposure behind it: a taker fill
    // is a decision, not an arrival, and counting it would put a fill in the
    // numerator with nothing in the denominator.
    CHECK(total_fills <= bt.result().fills);
}

}  // namespace

int main() {
    test_it_recovers_a_known_intensity_curve();
    test_k_is_per_dollar_not_per_tick();
    test_empty_buckets_are_dropped_and_counted();
    test_the_fit_is_weighted_by_fill_count();
    test_a_fit_needs_at_least_two_points();
    test_two_points_are_refused_because_they_cannot_fail();
    test_the_touch_is_judged_against_a_fit_it_did_not_influence();
    test_the_touch_misfit_shows_up_as_a_residual();
    test_exposure_is_per_order_second();
    test_untradable_time_is_excluded();
    test_an_unusable_mid_is_excluded();
    test_hidden_size_is_not_exposed();
    test_a_dead_entry_is_not_exposed();
    test_depth_is_integrated_as_the_mid_moves();
    test_deep_orders_pool_into_an_overflow_bucket();
    test_a_fill_is_bucketed_at_its_own_depth();
    test_crossed_observations_are_counted_not_hidden();
    test_a_real_run_produces_exposure_and_fills();
    return REPORT();
}
