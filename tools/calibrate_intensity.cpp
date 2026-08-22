// calibrate_intensity — measure lambda(delta) from our own fills.
//
// Avellaneda-Stoikov needs A and k in lambda(delta) = A*exp(-k*delta). Everyone
// assumes them. This measures them, which is possible only because the queue
// model knows which of our orders actually filled and how long each was
// genuinely exposed at each depth.
//
// PER LANE, WHICH IS A DECISION AND NOT A DEFAULT. lambda_hat is measured
// THROUGH a fill model, so A and k are properties of (this feed, this strategy,
// that model). Calibrating once under one model and running four would leave
// three lanes using parameters fitted in a world they do not live in --
// reintroducing exactly the cross-contamination the closed-loop design removed.
// So this fits each model separately and reports four curves. The reasoning is
// in docs/build-plan-9-12.md section 11.2; the decision is not left implicit.
//
// A SEPARATE RUN PER LANE, not four lanes sharing one strategy. Once the
// strategy sees its fills, the lanes are four different worlds with four
// different intent streams, so the exposure denominators genuinely differ. That
// is the whole point of phase 11.0 and it is why this is four passes over the
// feed rather than one.
//
// Usage:
//   calibrate_intensity <feed.gz> [--gamma X] [--k X] [--quote-size N]
//                       [--max-depth-ticks N] [--json out.json]
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/sim/as_maker.hpp"
#include "itchbook/sim/closed_loop.hpp"
#include "itchbook/sim/intensity.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

struct LaneCalibration {
    Model model;
    IntensityFit fit;
    std::vector<DepthBucket> buckets;
    std::vector<FitResidual> residuals;
    uint64_t maker_fills = 0;
    uint64_t total_fills = 0;
    double total_exposure = 0.0;
    uint64_t crossed = 0;
    uint64_t untradable_ns = 0;
    LaneResult result;
};

LaneCalibration run_lane(const char* feed, Model m, AsConfig cfg, IntensityConfig icfg) {
    ClosedLoopBacktest<AsMaker> bt(AsMaker{cfg}, m, FeeSchedule{}, LatencyModel{},
                                   RiskLimits{}, risk::KillSwitchConfig{},
                                   SessionClock{}, icfg);
    Reader reader(feed);
    parse(reader, bt);

    LaneCalibration out;
    out.model = m;
    out.buckets = bt.intensity().buckets();
    out.fit = fit_intensity(out.buckets, icfg.tick, &out.residuals);
    out.crossed = bt.intensity().crossed_observations();
    out.untradable_ns = bt.intensity().untradable_ns();
    out.result = bt.result();
    out.total_fills = out.result.fills;
    for (const DepthBucket& b : out.buckets) {
        out.maker_fills += b.fills;
        out.total_exposure += b.exposure_seconds;
    }
    return out;
}

void print_lane(const LaneCalibration& c) {
    std::printf("\n=== %s ===\n", to_string(c.model));
    std::printf("%-28s %14" PRIu64 "\n", "fills (all)", c.total_fills);
    std::printf("%-28s %14" PRIu64 "\n", "fills with exposure (maker)", c.maker_fills);
    std::printf("%-28s %14.1f\n", "exposure (order-seconds)", c.total_exposure);
    std::printf("%-28s %14" PRIu64 "\n", "crossed observations", c.crossed);
    std::printf("%-28s %14.1f\n", "untradable seconds",
                static_cast<double>(c.untradable_ns) / 1e9);
    if (!c.fit.ok) {
        std::printf("\n  NO FIT: %zu usable buckets (need 3). %zu had exposure but no\n"
                    "  fills, which is information -- it means nothing traded that deep.\n",
                    c.fit.points, c.fit.dropped_no_fills);
        return;
    }
    std::printf("\n%-28s %14.4f\n", "A (fills/order-second)", c.fit.A);
    std::printf("%-28s %14.1f\n", "k (per dollar)", c.fit.k);
    std::printf("%-28s %14.4f\n", "R^2 (weighted)", c.fit.r_squared);
    std::printf("%-28s %14zu\n", "buckets fitted", c.fit.points);
    std::printf("%-28s %14zu\n", "buckets with no fills", c.fit.dropped_no_fills);

    std::printf("\n%6s %14s %10s %12s %10s %10s\n", "ticks", "order-sec", "fills",
                "lambda", "ln obs", "residual");
    for (const FitResidual& r : c.residuals) {
        std::printf("%6d %14.1f %10" PRIu64 " %12.6f %10.3f %10.3f\n",
                    r.depth_ticks, r.exposure_seconds, r.fills,
                    r.exposure_seconds > 0
                        ? static_cast<double>(r.fills) / r.exposure_seconds
                        : 0.0,
                    r.observed_ln_lambda, r.residual);
    }
    // The touch misfit, named rather than left in a column. A-S assumes fill
    // intensity depends only on depth; at depth zero it depends mostly on queue
    // position, which the model has no way to express.
    for (const FitResidual& r : c.residuals) {
        if (r.depth_ticks == 0 && r.residual < -0.3) {
            std::printf("\n  The touch bucket sits %.2f log-units BELOW the fitted curve.\n"
                        "  That is the known A-S limitation: at depth zero, fill intensity\n"
                        "  is dominated by queue position rather than by depth, and the\n"
                        "  exponential has no way to express it.\n",
                        -r.residual);
        }
    }
}

bool write_json(const char* path, const std::vector<LaneCalibration>& lanes,
                const AsConfig& cfg, const char* feed, const char* symbol,
                const char* day) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n  \"feed\": \"%s\",\n", feed);
    // What this calibration IS, so a consumer can check it rather than trust a
    // filename. The experiment driver refuses a calibration whose day is not
    // the day it was told to hold out, and it cannot make that check unless the
    // artifact says which day it came from.
    std::fprintf(f, "  \"symbol\": \"%s\",\n  \"day\": \"%s\",\n", symbol, day);
    std::fprintf(f, "  \"strategy\": \"avellaneda-stoikov\",\n");
    std::fprintf(f, "  \"gamma\": %.6f,\n  \"k_assumed\": %.3f,\n", cfg.gamma, cfg.k);
    std::fprintf(f, "  \"quote_size\": %u,\n", cfg.quote_size);
    // Recorded because the numbers below are conditional on it. See 11.2.4.
    std::fprintf(f, "  \"calibrated_per_lane\": true,\n");
    std::fprintf(f, "  \"lanes\": {");
    bool first = true;
    for (const LaneCalibration& c : lanes) {
        std::fprintf(f, "%s\n    \"%s\": {\n", first ? "" : ",", to_string(c.model));
        first = false;
        std::fprintf(f, "      \"fit_ok\": %s,\n", c.fit.ok ? "true" : "false");
        std::fprintf(f, "      \"A\": %.8f,\n      \"k\": %.4f,\n"
                        "      \"r_squared\": %.6f,\n",
                     c.fit.A, c.fit.k, c.fit.r_squared);
        std::fprintf(f, "      \"buckets_fitted\": %zu,\n"
                        "      \"buckets_no_fills\": %zu,\n"
                        "      \"dof\": %zu,\n",
                     c.fit.points, c.fit.dropped_no_fills, c.fit.dof);
        // The touch judged against a fit it did not influence. Without this the
        // section 6.2 claim cannot be tested: the touch bucket carries most of
        // the Poisson weight and drags the line onto itself.
        std::fprintf(f, "      \"touch_excluded_ok\": %s,\n"
                        "      \"k_ex_touch\": %.4f,\n"
                        "      \"points_ex_touch\": %zu,\n"
                        "      \"touch_residual_ex\": %.6f,\n",
                     c.fit.touch_excluded_ok ? "true" : "false", c.fit.k_ex_touch,
                     c.fit.points_ex_touch, c.fit.touch_residual_ex);
        std::fprintf(f, "      \"maker_fills\": %" PRIu64 ",\n"
                        "      \"total_fills\": %" PRIu64 ",\n"
                        "      \"exposure_order_seconds\": %.3f,\n"
                        "      \"crossed_observations\": %" PRIu64 ",\n",
                     c.maker_fills, c.total_fills, c.total_exposure, c.crossed);
        std::fprintf(f, "      \"buckets\": [");
        bool fb = true;
        for (const DepthBucket& b : c.buckets) {
            if (b.exposure_seconds <= 0.0 && b.fills == 0) continue;
            std::fprintf(f, "%s\n        {\"ticks\": %d, \"exposure\": %.4f, "
                            "\"fills\": %" PRIu64 ", \"shares\": %" PRIu64 "}",
                         fb ? "" : ",", b.depth_ticks, b.exposure_seconds, b.fills,
                         b.shares);
            fb = false;
        }
        std::fprintf(f, "\n      ],\n      \"residuals\": [");
        bool fr = true;
        for (const FitResidual& r : c.residuals) {
            std::fprintf(f, "%s\n        {\"ticks\": %d, \"observed\": %.6f, "
                            "\"fitted\": %.6f, \"residual\": %.6f, "
                            "\"fills\": %" PRIu64 "}",
                         fr ? "" : ",", r.depth_ticks, r.observed_ln_lambda,
                         r.fitted_ln_lambda, r.residual, r.fills);
            fr = false;
        }
        std::fprintf(f, "\n      ]\n    }");
    }
    std::fprintf(f, "\n  }\n}\n");
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed = nullptr;
    const char* json = nullptr;
    std::string symbol = "UNKNOWN";
    std::string day = "UNKNOWN";
    AsConfig cfg;
    IntensityConfig icfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--symbol") symbol = next("--symbol");
        else if (a == "--day") day = next("--day");
        else if (a == "--gamma") cfg.gamma = std::atof(next("--gamma"));
        else if (a == "--k") cfg.k = std::atof(next("--k"));
        else if (a == "--quote-size")
            cfg.quote_size = static_cast<uint32_t>(std::atoi(next("--quote-size")));
        else if (a == "--max-depth-ticks")
            icfg.max_depth_ticks = std::atoi(next("--max-depth-ticks"));
        else if (a == "--json") json = next("--json");
        else if (feed == nullptr) feed = argv[i];
        else {
            std::fprintf(stderr,
                "usage: %s <feed.gz> [--symbol S] [--day D] [--gamma X] [--k X]\n"
                "       [--quote-size N] [--max-depth-ticks N] [--json out.json]\n",
                argv[0]);
            return 2;
        }
    }
    if (feed == nullptr) {
        std::fprintf(stderr, "usage: %s <feed.gz> [options]\n", argv[0]);
        return 2;
    }

    std::printf("calibrating lambda(delta) = A*exp(-k*delta) from our own fills\n");
    std::printf("feed: %s\n", feed);
    std::printf("quoting with gamma=%g, assumed k=%g (the value being replaced)\n",
                cfg.gamma, cfg.k);
    std::printf("\nONE RUN PER FILL MODEL. Once a strategy sees its fills the lanes are\n"
                "four different worlds with four different intent streams, so each has\n"
                "its own exposure denominator. A and k below are conditional on the\n"
                "model that produced them; see docs/build-plan-9-12.md section 11.2.\n");

    std::vector<LaneCalibration> lanes;
    try {
        for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
            lanes.push_back(run_lane(feed, m, cfg, icfg));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    for (const LaneCalibration& c : lanes) print_lane(c);

    std::printf("\n=== the four worlds, side by side ===\n");
    // dof is printed beside R^2 and not behind it. R^2 = 1.0000 on two points
    // is not a perfect fit, it is a line through two dots, and reporting the
    // one without the other is how that gets mistaken for the best row here.
    std::printf("%-14s %10s %10s %8s %5s %8s %10s %12s\n", "model", "A", "k", "R^2",
                "dof", "k noTch", "fills", "order-sec");
    for (const LaneCalibration& c : lanes) {
        if (!c.fit.ok) {
            std::printf("%-14s %10s %10s %8s %5zu %8s %10" PRIu64 " %12.1f   "
                        "NOT FITTED (%zu point(s))\n",
                        to_string(c.model), "-", "-", "-", c.fit.dof, "-",
                        c.maker_fills, c.total_exposure, c.fit.points);
            continue;
        }
        char notch[16];
        if (c.fit.touch_excluded_ok) {
            std::snprintf(notch, sizeof notch, "%.1f", c.fit.k_ex_touch);
        } else {
            std::snprintf(notch, sizeof notch, "%s", "-");
        }
        std::printf("%-14s %10.4f %10.1f %8.4f %5zu %8s %10" PRIu64 " %12.1f\n",
                    to_string(c.model), c.fit.A, c.fit.k, c.fit.r_squared, c.fit.dof,
                    notch, c.maker_fills, c.total_exposure);
    }
    // The section 6.2 test, run only where it is not circular.
    for (const LaneCalibration& c : lanes) {
        if (!c.fit.touch_excluded_ok) continue;
        std::printf("\n%s: against a curve fitted WITHOUT it (%zu deep buckets, "
                    "k = %.1f),\n  the touch sits %.2f in ln lambda %s it.\n",
                    to_string(c.model), c.fit.points_ex_touch, c.fit.k_ex_touch,
                    std::fabs(c.fit.touch_residual_ex),
                    c.fit.touch_residual_ex < 0 ? "BELOW" : "ABOVE");
    }
    std::printf("\nThe spread between these k values is the cost of assuming one. A\n"
                "strategy calibrated in one world and run in another is using a fill\n"
                "curve from a market it does not live in.\n");

    if (json != nullptr && !write_json(json, lanes, cfg, feed, symbol.c_str(),
                                       day.c_str())) {
        std::fprintf(stderr, "error: cannot write %s\n", json);
        return 1;
    }
    return 0;
}
