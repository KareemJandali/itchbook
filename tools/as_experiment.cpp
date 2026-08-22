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
                const AsConfig& base, uint64_t latency_ns) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n  \"feed\": \"%s\",\n  \"symbol\": \"%s\",\n  \"day\": \"%s\",\n",
                 feed, symbol, day);
    std::fprintf(f, "  \"k_assumed\": %.4f,\n  \"quote_size\": %u,\n"
                    "  \"latency_ns\": %" PRIu64 ",\n",
                 base.k, base.quote_size, latency_ns);
    std::fprintf(f, "  \"runs\": [");
    bool first = true;
    for (const ArmResult& r : rows) {
        std::fprintf(f,
            "%s\n    {\"arm\": \"%s\", \"gamma\": %.6f, \"model\": \"%s\","
            " \"equity_per_share_micros\": %" PRId64 ", \"equity_micros\": %" PRId64 ","
            " \"fills\": %" PRIu64 ", \"shares\": %" PRIu64 ","
            " \"fees_micros\": %" PRId64 ", \"residual_position\": %" PRId64 ","
            " \"inv_mean\": %.4f, \"inv_stdev\": %.4f, \"inv_max_abs\": %" PRId64 ","
            " \"inv_max_long\": %" PRId64 ", \"inv_max_short\": %" PRId64 ","
            " \"markout_100ms\": %" PRId64 ", \"markout_1s\": %" PRId64 ","
            " \"markout_10s\": %" PRId64 ", \"unresolved_10s\": %" PRIu64 ","
            " \"suppressed_quotes\": %" PRIu64 ", \"trip\": \"%s\"}",
            first ? "" : ",", r.arm.c_str(), r.gamma, to_string(r.model),
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
        else if (a == "--quote-size")
            base.quote_size = static_cast<uint32_t>(std::atoi(next("--quote-size")));
        else if (a == "--latency-ns")
            latency_ns = std::strtoull(next("--latency-ns"), nullptr, 10);
        else if (a == "--json") json = next("--json");
        else if (feed == nullptr) feed = argv[i];
        else {
            std::fprintf(stderr,
                "usage: %s <feed.gz> --symbol S --day D [--gammas a,b,c] [--k X]\n"
                "       [--quote-size N] [--latency-ns N] [--json out.json]\n", argv[0]);
            return 2;
        }
    }
    if (feed == nullptr) {
        std::fprintf(stderr, "usage: %s <feed.gz> --symbol S --day D [options]\n", argv[0]);
        return 2;
    }

    LatencyModel lat;
    lat.order_ns = latency_ns;
    lat.cancel_ns = latency_ns;

    std::printf("symbol %s, day %s, feed %s\n", symbol.c_str(), day.c_str(), feed);
    std::printf("latency %" PRIu64 " ns, quote size %u, assumed k %g\n\n",
                latency_ns, base.quote_size, base.k);
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
            print_row(rows.back());

            AsConfig control = base;
            control.gamma = 0.0;
            rows.push_back(run_one(feed, AsMaker{control}, m, lat, "as-gamma0", 0.0));
            print_row(rows.back());

            for (double g : parse_gammas(gammas)) {
                AsConfig c = base;
                c.gamma = g;
                rows.push_back(run_one(feed, AsMaker{c}, m, lat, "as", g));
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
                                       base, latency_ns)) {
        std::fprintf(stderr, "error: cannot write %s\n", json);
        return 1;
    }
    return 0;
}
