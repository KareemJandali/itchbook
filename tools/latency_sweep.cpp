// latency_sweep — P&L against the one number nobody can measure for you.
//
// The build plan asks for it directly: "Your order takes N microseconds to
// reach the exchange. The book moves in between. Make N configurable and show
// P&L sensitivity to it." A single backtest at one latency is a point estimate
// dressed up as a result; the useful object is the curve, because its SHAPE is
// what tells you whether a strategy is a real edge or a latency artefact.
//
// Two shapes, and they mean opposite things:
//
//   * **Flat.** P&L barely moves as flight time grows. The strategy is not
//     racing anyone. Whatever it earns, it earns for some other reason, and the
//     result survives a slower machine.
//   * **Steep, collapsing to zero.** All of the P&L lived in the first few
//     hundred microseconds. That is a latency edge, and claiming it requires
//     owning the infrastructure to hold it. Most retail-adjacent "alpha" that
//     evaporates by 1ms was never alpha.
//
// The feed is decoded once and replayed from memory for every latency point, so
// the sweep costs one gzip pass rather than one per point, and every point sees
// byte-identical input.
//
// Usage:
//   latency_sweep <feed.gz> [--strategy touch-maker|patient-maker|crosser|far-quoter]
//                 [--size N] [--fees base|top|inverted]
//                 [--points 0,100000,250000,1000000,...] [--csv out.csv]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/sim/backtest.hpp"
#include "itchbook/sim/strategies.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

double cents(Money micros) { return static_cast<double>(micros) / 10000.0; }

// The whole feed, framed once. Messages are kept in one flat buffer rather than
// a vector-of-vectors: the sweep touches it repeatedly and the alternative is a
// pointer chase per message for no reason.
struct Feed {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offset;
    std::vector<uint16_t> length;

    void on_message(char, const uint8_t* p, uint16_t len) {
        offset.push_back(static_cast<uint32_t>(bytes.size()));
        length.push_back(len);
        bytes.insert(bytes.end(), p, p + len);
    }
    size_t size() const { return offset.size(); }
};

// One row of the sweep: everything that varies with latency, for one model.
struct Point {
    uint64_t latency_ns = 0;
    Model model = Model::Naive;
    uint64_t fills = 0;
    uint64_t shares = 0;
    Money equity = 0;
    Money per_share = 0;
    Money edge_per_share = 0;
    Money fees_per_share = 0;
    Money drift_1s = 0;
};

template <typename S>
std::vector<Point> sweep(const Feed& feed, S strategy, FeeSchedule fees,
                         const std::vector<uint64_t>& latencies) {
    std::vector<Point> out;
    for (uint64_t ns : latencies) {
        Backtest<S> bt(strategy, fees, LatencyModel::uniform(ns));
        for (size_t i = 0; i < feed.size(); ++i) {
            const uint8_t* p = feed.bytes.data() + feed.offset[i];
            bt.on_message(static_cast<char>(*p), p, feed.length[i]);
        }
        for (const LaneResult& r : bt.results()) {
            Point pt;
            pt.latency_ns = ns;
            pt.model = r.model;
            pt.fills = r.fills;
            pt.shares = r.shares;
            pt.equity = r.equity;
            pt.per_share = r.equity_per_share;
            const auto n = static_cast<Money>(r.shares);
            pt.edge_per_share = r.shares ? divide_round(r.edge, n) : 0;
            pt.fees_per_share = r.shares ? divide_round(r.fees, n) : 0;
            pt.drift_1s = r.markouts[1].drift_per_share;
            out.push_back(pt);
        }
        std::fprintf(stderr, "  %8" PRIu64 " us done\n", ns / 1000);
    }
    return out;
}

void print_table(const std::vector<Point>& pts, const char* strategy,
                 const std::vector<uint64_t>& latencies) {
    std::printf("\nfills by latency (shares), strategy: %s\n", strategy);
    std::printf("%-12s", "model");
    for (uint64_t ns : latencies) std::printf("%12" PRIu64 "us", ns / 1000);
    std::printf("\n");
    for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
        std::printf("%-12s", to_string(m));
        for (uint64_t ns : latencies) {
            for (const Point& p : pts) {
                if (p.model == m && p.latency_ns == ns) {
                    std::printf("%14" PRIu64, p.shares);
                }
            }
        }
        std::printf("\n");
    }

    std::printf("\nP&L per share (cents) by latency\n");
    std::printf("%-12s", "model");
    for (uint64_t ns : latencies) std::printf("%12" PRIu64 "us", ns / 1000);
    std::printf("\n");
    for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
        std::printf("%-12s", to_string(m));
        for (uint64_t ns : latencies) {
            for (const Point& p : pts) {
                if (p.model == m && p.latency_ns == ns) {
                    std::printf("%14.4f", cents(p.per_share));
                }
            }
        }
        std::printf("\n");
    }
    std::printf(
        "\nA flat row is a strategy that is not racing anyone. A row that "
        "collapses\ntowards zero as latency grows had its P&L in the first few "
        "hundred us, and\nkeeping it means owning the infrastructure to stay "
        "there.\n");
}

void write_csv(const char* path, const std::vector<Point>& pts, const char* strategy) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", path);
        return;
    }
    // Micros throughout, so the reader never has to undo a rounding we did.
    std::fprintf(f,
                 "strategy,latency_ns,model,fills,shares,equity_micros,"
                 "per_share_micros,edge_per_share_micros,fees_per_share_micros,"
                 "drift_1s_micros\n");
    for (const Point& p : pts) {
        std::fprintf(f,
                     "%s,%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64
                     ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                     strategy, p.latency_ns, to_string(p.model), p.fills, p.shares,
                     p.equity, p.per_share, p.edge_per_share, p.fees_per_share,
                     p.drift_1s);
    }
    std::fclose(f);
}

std::vector<uint64_t> parse_points(const std::string& s) {
    std::vector<uint64_t> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        out.push_back(std::strtoull(s.substr(i, j - i).c_str(), nullptr, 10));
        i = j + 1;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed_path = nullptr;
    std::string strategy = "touch-maker";
    std::string fee_tier = "base";
    const char* csv_path = nullptr;
    uint32_t size = 100;
    // Zero first as the reference, then a colocated round trip, then the range
    // where a retail path actually lives. Zero is included so the curve shows
    // exactly how much the flattering assumption was worth.
    std::vector<uint64_t> latencies{0, 100000, 250000, 500000, 1000000, 5000000};

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strategy" && i + 1 < argc) strategy = argv[++i];
        else if (a == "--fees" && i + 1 < argc) fee_tier = argv[++i];
        else if (a == "--csv" && i + 1 < argc) csv_path = argv[++i];
        else if (a == "--size" && i + 1 < argc) size = static_cast<uint32_t>(std::atol(argv[++i]));
        else if (a == "--points" && i + 1 < argc) latencies = parse_points(argv[++i]);
        else if (feed_path == nullptr) feed_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (feed_path == nullptr || latencies.empty()) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> [--strategy touch-maker|patient-maker|crosser|far-quoter]\n"
                     "                    [--size N] [--fees base|top|inverted]\n"
                     "                    [--points ns,ns,...] [--csv out.csv]\n",
                     argv[0]);
        return 2;
    }

    Feed feed;
    try {
        Reader reader(feed_path);
        parse(reader, feed);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "loaded %zu messages, sweeping %zu latency points\n", feed.size(),
                 latencies.size());

    FeeSchedule fees;
    if (fee_tier == "top") fees = nasdaq_top_tier_2019();
    else if (fee_tier == "inverted") fees = inverted_venue_2019();

    std::vector<Point> pts;
    if (strategy == "touch-maker") {
        TouchMaker s;
        s.size = size;
        pts = sweep(feed, s, fees, latencies);
    } else if (strategy == "patient-maker") {
        PatientMaker s;
        s.size = size;
        pts = sweep(feed, s, fees, latencies);
    } else if (strategy == "crosser") {
        Crosser s;
        s.size = size;
        pts = sweep(feed, s, fees, latencies);
    } else if (strategy == "far-quoter") {
        FarQuoter s;
        s.size = size;
        pts = sweep(feed, s, fees, latencies);
    } else {
        std::fprintf(stderr, "error: unknown strategy '%s'\n", strategy.c_str());
        return 2;
    }

    print_table(pts, strategy.c_str(), latencies);
    if (csv_path != nullptr) write_csv(csv_path, pts, strategy.c_str());
    return 0;
}
