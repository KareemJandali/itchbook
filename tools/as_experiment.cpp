// as_experiment — one symbol-day, every arm, as JSON.
//
// The phase 11.3 question is small and honest: does inventory-aware quoting lose
// less than naive symmetric quoting, and THROUGH WHICH MECHANISM -- fewer toxic
// fills, or smaller inventory excursions?
//
// THREE ARMS, BECAUSE TWO CANNOT ANSWER IT.
//
//   symmetric-touch   phase 6's maker: quote the touch, ignore inventory.
//   as-gamma0         A-S with the inventory skew turned OFF. Same spread
//                     formula, same re-quote discipline, same size.
//   as                A-S proper, at each swept gamma.
//
// The middle arm is the control and it is the reason this tool exists rather
// than a two-way comparison. A-S differs from a touch-maker in inventory
// awareness AND in where it quotes AND in how often; an improvement over the
// touch-maker could come from any of them. Comparing A-S against gamma = 0
// holds everything fixed except the skew, so "A-S beats a naive maker" and "the
// skew is what beat it" stay separable -- and they are routinely reported as
// one claim.
//
// FOUR LANES PER ARM, run as four separate closed loops. Since 11.0 the lanes
// are four worlds rather than four gradings, so every headline number is a band
// and no arm gets a single figure.
//
// PER SYMBOL, NEVER AGGREGATED. This tool does ONE symbol-day and says which.
// Pooling symbols is how a result driven entirely by one liquid name gets
// reported as a property of a strategy, and the plan forbids aggregate-only
// tables for that reason. Combining across symbol-days is the Python driver's
// job, and it keeps the rows.
//
// Usage:
//   as_experiment <feed.gz> --symbol MSFT --day 2019-12-30 [--gammas a,b,c]
//                 [--quote-size N] [--k X] [--latency-ns N] [--json out.json]
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/sim/as_maker.hpp"
#include "itchbook/sim/closed_loop.hpp"
#include "itchbook/sim/inventory_strategies.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

struct ArmResult {
    std::string arm;
    double gamma = 0.0;
    // The k this row was actually run with. Recorded per row rather than once
    // per file because k is now per LANE: phase 11.2 fits it through a queue
    // model, so the four fill models produce four different curves, and a
    // single file-level k would silently discard three of them.
    double k = 0.0;
    Model model = Model::Naive;
    LaneResult lane;
    double inv_mean = 0.0;
    double inv_stdev = 0.0;
    int64_t inv_max_abs = 0;
    int64_t inv_max_long = 0;
    int64_t inv_max_short = 0;
    uint64_t quotes = 0;
    uint64_t requotes = 0;
    uint64_t late_quotes_with_inventory = 0;
};

template <typename Strategy>
ArmResult run_one(const char* feed, Strategy s, Model m, LatencyModel lat,
                  const std::string& arm, double gamma) {
    ClosedLoopBacktest<Strategy> bt(s, m, FeeSchedule{}, lat);
    Reader reader(feed);
    parse(reader, bt);
    ArmResult r;
    r.arm = arm;
    r.gamma = gamma;
    r.model = m;
    r.lane = bt.result();
    r.inv_mean = bt.inventory().mean();
    r.inv_stdev = std::sqrt(bt.inventory().variance());
    r.inv_max_abs = bt.inventory().max_abs();
    r.inv_max_long = bt.inventory().max_long();
    r.inv_max_short = bt.inventory().max_short();
    return r;
}

// k per fill model. Four separate fits, because lambda-hat is estimated
// THROUGH a queue model and is therefore conditional on it -- that is the
// paper's section 6.1 decision, and collapsing it back to one number here
// would make the paper claim a conditioning it does not perform.
struct LaneK {
    double k[4] = {0, 0, 0, 0};
    bool measured = false;

    static int index(Model m) {
        switch (m) {
            case Model::Naive: return 0;
            case Model::Optimistic: return 1;
            case Model::Mbo: return 2;
            default: return 3;
        }
    }
    double get(Model m) const { return k[index(m)]; }
    void set_all(double v) { for (double& x : k) x = v; }
};

// "naive=90.1,optimistic=145.0,mbo=200.4,pessimistic=255.9". All four are
// required: a missing lane is a lane whose intensity was never fitted, and
// filling it in from another lane's number is exactly the collapse this flag
// exists to prevent.
bool parse_k_per_lane(const std::string& csv, LaneK* out) {
    bool seen[4] = {false, false, false, false};
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string piece = csv.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty()) {
            const size_t eq = piece.find('=');
            if (eq == std::string::npos) return false;
            const std::string name = piece.substr(0, eq);
            const double v = std::atof(piece.c_str() + eq + 1);
            if (!(v > 0.0)) return false;
            int idx = -1;
            if (name == "naive") idx = 0;
            else if (name == "optimistic") idx = 1;
            else if (name == "mbo") idx = 2;
            else if (name == "pessimistic") idx = 3;
            else return false;
            out->k[idx] = v;
            seen[idx] = true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    for (bool s : seen) if (!s) return false;
    out->measured = true;
    return true;
}

std::vector<double> parse_gammas(const std::string& csv) {
    std::vector<double> out;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string piece = csv.substr(start, comma == std::string::npos
                                                        ? std::string::npos
                                                        : comma - start);
        if (!piece.empty()) out.push_back(std::atof(piece.c_str()));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

void print_row(const ArmResult& r) {
    std::printf("%-16s %8.4f %-12s %10" PRId64 " %8" PRIu64 " %10.1f %8" PRId64
                " %10" PRId64 " %10" PRId64 "\n",
                r.arm.c_str(), r.gamma, to_string(r.model), r.lane.equity_per_share,
                r.lane.fills, r.inv_stdev, r.inv_max_abs,
                r.lane.markouts[1].markout_per_share,
                r.lane.markouts[2].markout_per_share);
}

bool write_json(const char* path, const std::vector<ArmResult>& rows,
                const char* feed, const char* symbol, const char* day,
                const AsConfig& base, uint64_t latency_ns, const LaneK& kl) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n  \"feed\": \"%s\",\n  \"symbol\": \"%s\",\n  \"day\": \"%s\",\n",
                 feed, symbol, day);
    // k_source is the load-bearing field. A reader must be able to tell a run
    // that used measured per-lane intensity from one that used the placeholder,
    // without cross-referencing anything.
    std::fprintf(f, "  \"k_source\": \"%s\",\n",
                 kl.measured ? "measured-per-lane" : "assumed-scalar");
    std::fprintf(f, "  \"k_per_lane\": {\"naive\": %.4f, \"optimistic\": %.4f,"
                    " \"mbo\": %.4f, \"pessimistic\": %.4f},\n",
                 kl.k[0], kl.k[1], kl.k[2], kl.k[3]);
    std::fprintf(f, "  \"k_assumed\": %.4f,\n  \"quote_size\": %u,\n"
                    "  \"latency_ns\": %" PRIu64 ",\n",
                 base.k, base.quote_size, latency_ns);
    std::fprintf(f, "  \"runs\": [");
    bool first = true;
    for (const ArmResult& r : rows) {
        std::fprintf(f,
            "%s\n    {\"arm\": \"%s\", \"gamma\": %.6f, \"model\": \"%s\","
            " \"k\": %.4f,"
            " \"equity_per_share_micros\": %" PRId64 ", \"equity_micros\": %" PRId64 ","
            " \"fills\": %" PRIu64 ", \"shares\": %" PRIu64 ","
            " \"fees_micros\": %" PRId64 ", \"residual_position\": %" PRId64 ","
            " \"inv_mean\": %.4f, \"inv_stdev\": %.4f, \"inv_max_abs\": %" PRId64 ","
            " \"inv_max_long\": %" PRId64 ", \"inv_max_short\": %" PRId64 ","
            " \"markout_100ms\": %" PRId64 ", \"markout_1s\": %" PRId64 ","
            " \"markout_10s\": %" PRId64 ", \"unresolved_10s\": %" PRIu64 ","
            " \"suppressed_quotes\": %" PRIu64 ", \"trip\": \"%s\"}",
            first ? "" : ",", r.arm.c_str(), r.gamma, to_string(r.model), r.k,
            r.lane.equity_per_share, r.lane.equity, r.lane.fills, r.lane.shares,
            r.lane.fees, r.lane.residual_position, r.inv_mean, r.inv_stdev,
            r.inv_max_abs, r.inv_max_long, r.inv_max_short,
            r.lane.markouts[0].markout_per_share, r.lane.markouts[1].markout_per_share,
            r.lane.markouts[2].markout_per_share, r.lane.markouts[2].unresolved_fills,
            r.lane.suppressed_quotes, risk::to_string(r.lane.trip));
        first = false;
    }
    std::fprintf(f, "\n  ]\n}\n");
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed = nullptr;
    const char* json = nullptr;
    std::string symbol = "UNKNOWN";
    std::string day = "UNKNOWN";
    std::string gammas = "0.001,0.005,0.02,0.1";
    uint64_t latency_ns = 0;
    AsConfig base;
    LaneK kl;
    const char* k_per_lane = nullptr;

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
        else if (a == "--gammas") gammas = next("--gammas");
        else if (a == "--k") base.k = std::atof(next("--k"));
        else if (a == "--k-per-lane") k_per_lane = next("--k-per-lane");
        else if (a == "--quote-size")
            base.quote_size = static_cast<uint32_t>(std::atoi(next("--quote-size")));
        else if (a == "--latency-ns")
            latency_ns = std::strtoull(next("--latency-ns"), nullptr, 10);
        else if (a == "--json") json = next("--json");
        else if (feed == nullptr) feed = argv[i];
        else {
            std::fprintf(stderr,
                "usage: %s <feed.gz> --symbol S --day D [--gammas a,b,c]\n"
                "       [--k X | --k-per-lane naive=A,optimistic=B,mbo=C,pessimistic=D]\n"
                "       [--quote-size N] [--latency-ns N] [--json out.json]\n", argv[0]);
            return 2;
        }
    }
    if (feed == nullptr) {
        std::fprintf(stderr, "usage: %s <feed.gz> --symbol S --day D [options]\n", argv[0]);
        return 2;
    }

    // Per-lane k if it was measured; otherwise the placeholder, in every lane,
    // and the banner and the JSON both say so.
    if (k_per_lane != nullptr) {
        if (!parse_k_per_lane(k_per_lane, &kl)) {
            std::fprintf(stderr,
                "error: --k-per-lane wants all four lanes with positive values, as\n"
                "       naive=A,optimistic=B,mbo=C,pessimistic=D\n"
                "       got: %s\n", k_per_lane);
            return 2;
        }
    } else {
        kl.set_all(base.k);
    }

    LatencyModel lat;
    lat.order_ns = latency_ns;
    lat.cancel_ns = latency_ns;

    std::printf("symbol %s, day %s, feed %s\n", symbol.c_str(), day.c_str(), feed);
    std::printf("latency %" PRIu64 " ns, quote size %u\n", latency_ns, base.quote_size);
    if (kl.measured) {
        std::printf("k MEASURED per lane: naive %g, optimistic %g, mbo %g, "
                    "pessimistic %g\n\n", kl.k[0], kl.k[1], kl.k[2], kl.k[3]);
    } else {
        std::printf("k ASSUMED %g in every lane -- no calibration was supplied, so\n"
                    "the section 6.1 per-lane conditioning is NOT in force here.\n\n",
                    base.k);
    }
    std::printf("%-16s %8s %-12s %10s %8s %10s %8s %10s %10s\n", "arm", "gamma",
                "model", "eq/share", "fills", "inv sd", "inv max", "mk 1s", "mk 10s");

    std::vector<ArmResult> rows;
    try {
        for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
            SymmetricTouchMaker touch;
            touch.quote_size = base.quote_size;
            touch.tick = base.tick;
            touch.requote_ticks = base.requote_ticks;
            rows.push_back(run_one(feed, touch, m, lat, "symmetric-touch", 0.0));
            // k = 0 for the touch arm and it is not a missing value: a maker
            // that quotes the touch never evaluates the spread formula, so no
            // intensity curve enters this row. Writing the lane's k here would
            // claim an input the arm never read.
            rows.back().k = 0.0;
            print_row(rows.back());

            AsConfig control = base;
            control.gamma = 0.0;
            control.k = kl.get(m);
            rows.push_back(run_one(feed, AsMaker{control}, m, lat, "as-gamma0", 0.0));
            rows.back().k = control.k;
            print_row(rows.back());

            for (double g : parse_gammas(gammas)) {
                AsConfig c = base;
                c.gamma = g;
                c.k = kl.get(m);
                rows.push_back(run_one(feed, AsMaker{c}, m, lat, "as", g));
                rows.back().k = c.k;
                print_row(rows.back());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::printf("\nThree arms on purpose. symmetric-touch is the naive baseline;\n"
                "as-gamma0 is A-S with the inventory skew OFF and everything else\n"
                "held fixed. The gap from touch to gamma0 is the spread choice; the\n"
                "gap from gamma0 to as is the skew. Reporting only touch-vs-as would\n"
                "attribute both to inventory awareness.\n");
    std::printf("\nOne symbol-day. Pooling symbols is how a result driven by one\n"
                "liquid name becomes a claim about a strategy.\n");

    if (json != nullptr && !write_json(json, rows, feed, symbol.c_str(), day.c_str(),
                                       base, latency_ns, kl)) {
        std::fprintf(stderr, "error: cannot write %s\n", json);
        return 1;
    }
    return 0;
}
