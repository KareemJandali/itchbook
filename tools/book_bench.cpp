// book_bench — per-message latency for the book's hot path.
//
// Phase 4 starts with a baseline, because without one there is no story: "it
// got faster" is not a result, "p99 fell from X to Y and here is the mechanism"
// is. This measures the book mutation and nothing else.
//
// What is deliberately outside the measured region:
//   * gzip decompression and IO — the feed is decoded into memory first
//   * framing and length checks — measured separately, they are not the book
//   * every allocation and every print
//
// Two numbers are reported and they answer different questions:
//
//   Per-message percentiles come from an rdtsc pair around each handler. The
//   fences cost ~40-50 cycles, which is the same order as the work itself, so
//   the harness measures that overhead and subtracts it. Treat these as good
//   for comparison between runs, not as absolute truth.
//
//   Throughput comes from a second, completely uninstrumented replay. No fences
//   and no per-message stores, so it is the honest end-to-end figure.
//
// Usage: book_bench <feed.gz> [--json out.json] [--tick N] [--repeat N]
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/bench/histogram.hpp"
#include "itchbook/bench/rdtsc.hpp"
#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

using itchbook::bench::Histogram;

// The types worth reporting separately. A and D are ~92% of a real feed, so
// their numbers are effectively the whole story; the rest are listed because a
// regression in one of them would otherwise hide inside the average.
constexpr char kTypes[] = {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'Q'};
constexpr size_t kNumTypes = sizeof(kTypes);

int type_slot(char t) {
    for (size_t i = 0; i < kNumTypes; ++i) {
        if (kTypes[i] == t) return static_cast<int>(i);
    }
    return -1;
}

// The feed, decoded once and held in memory so the measured region touches
// nothing but the book.
struct Feed {
    std::vector<uint8_t> bytes;
    struct Msg {
        uint32_t offset;
        uint16_t len;
        char type;
    };
    std::vector<Msg> msgs;

    struct Loader {
        Feed* feed;
        void on_message(char type, const uint8_t* p, uint16_t len) {
            if (!itchbook::book::modelled(type)) return;
            auto off = static_cast<uint32_t>(feed->bytes.size());
            feed->bytes.insert(feed->bytes.end(), p, p + len);
            feed->msgs.push_back({off, len, type});
        }
    };
};

Feed load(const char* path) {
    Feed feed;
    feed.bytes.reserve(64u << 20);
    feed.msgs.reserve(4u << 20);
    itchbook::Reader reader(path);
    Feed::Loader loader{&feed};
    itchbook::parse(reader, loader);
    feed.bytes.shrink_to_fit();
    feed.msgs.shrink_to_fit();
    return feed;
}

// What an empty measurement costs. Subtracted from every sample below.
uint32_t measure_overhead() {
    Histogram h(20000);
    for (int i = 0; i < 20000; ++i) {
        uint64_t a = itchbook::bench::cycles_begin();
        uint64_t b = itchbook::bench::cycles_end();
        h.add(b - a);
    }
    h.finalize();
    return h.percentile(50);
}

struct Result {
    std::vector<Histogram> per_type;
    Histogram overall;
    uint64_t applied = 0;
    double throughput_ns_per_msg = 0.0;
    uint64_t wall_cycles = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* json_path = nullptr;
    int32_t tick = 100;
    int repeat = 3;
    int refs_bits = 20;   // ref map slots = 1 << refs_bits

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else if (a == "--tick" && i + 1 < argc) {
            tick = static_cast<int32_t>(std::atol(argv[++i]));
        } else if (a == "--repeat" && i + 1 < argc) {
            repeat = std::atoi(argv[++i]);
        } else if (a == "--refs-bits" && i + 1 < argc) {
            refs_bits = std::atoi(argv[++i]);
        } else if (path == nullptr) {
            path = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (path == nullptr) {
        std::fprintf(stderr, "usage: %s <feed.gz> [--json out.json] [--tick N] [--repeat N]\n",
                     argv[0]);
        return 2;
    }

    if (!itchbook::bench::tsc_is_invariant()) {
        std::fprintf(stderr,
                     "warning: no invariant TSC (constant_tsc + nonstop_tsc).\n"
                     "         Cycle counts will drift with frequency scaling.\n");
    }
    const double cycles_per_ns = itchbook::bench::calibrate_cycles_per_ns();

    std::printf("loading %s ...\n", path);
    Feed feed;
    try {
        feed = load(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("  %zu messages, %.1f MB in memory\n\n",
                feed.msgs.size(), static_cast<double>(feed.bytes.size()) / 1e6);

    const uint32_t overhead = measure_overhead();

    // ---- instrumented pass: per-message latency ----
    Result r{std::vector<Histogram>(), Histogram(feed.msgs.size()), 0, 0.0, 0};
    r.per_type.reserve(kNumTypes);
    for (size_t i = 0; i < kNumTypes; ++i) r.per_type.emplace_back(feed.msgs.size() / 2 + 16);

    {
        itchbook::book::Book book(tick, 20, size_t{1} << refs_bits);
        for (const auto& m : feed.msgs) {
            const uint8_t* p = feed.bytes.data() + m.offset;
            int slot = type_slot(m.type);

            uint64_t t0 = itchbook::bench::cycles_begin();
            bool applied = itchbook::book::apply(book, m.type, p);
            uint64_t t1 = itchbook::bench::cycles_end();

            uint64_t d = t1 - t0;
            d = d > overhead ? d - overhead : 0;
            r.overall.add(d);
            if (slot >= 0) r.per_type[static_cast<size_t>(slot)].add(d);
            r.applied += applied ? 1 : 0;
        }
    }

    // ---- clean pass: throughput, with no instrumentation at all ----
    {
        uint64_t best = UINT64_MAX;
        for (int run = 0; run < repeat; ++run) {
            itchbook::book::Book book(tick, 20, size_t{1} << refs_bits);
            uint64_t t0 = itchbook::bench::cycles_begin();
            for (const auto& m : feed.msgs) {
                itchbook::book::apply(book, m.type, feed.bytes.data() + m.offset);
            }
            uint64_t t1 = itchbook::bench::cycles_end();
            best = std::min(best, t1 - t0);
        }
        r.wall_cycles = best;
        r.throughput_ns_per_msg =
            static_cast<double>(best) / cycles_per_ns / static_cast<double>(feed.msgs.size());
    }

    r.overall.finalize();
    for (auto& h : r.per_type) h.finalize();

    // ---- report ----
    std::printf("calibration    %.4f cycles/ns    harness overhead %u cycles (subtracted)\n\n",
                cycles_per_ns, overhead);
    std::printf("%-6s %10s %8s %8s %8s %8s %10s\n",
                "type", "count", "p50", "p99", "p99.9", "max", "share");
    std::printf("---------------------------------------------------------------------\n");
    for (size_t i = 0; i < kNumTypes; ++i) {
        const Histogram& h = r.per_type[i];
        if (h.empty()) continue;
        double share = 100.0 * static_cast<double>(h.count()) /
                       static_cast<double>(feed.msgs.size());
        std::printf("%-6c %10zu %8u %8u %8u %8u %9.2f%%\n",
                    kTypes[i], h.count(), h.percentile(50), h.percentile(99),
                    h.percentile(99.9), h.max(), share);
    }
    std::printf("---------------------------------------------------------------------\n");
    std::printf("%-6s %10zu %8u %8u %8u %8u\n", "all", r.overall.count(),
                r.overall.percentile(50), r.overall.percentile(99),
                r.overall.percentile(99.9), r.overall.max());
    std::printf("\nthroughput (uninstrumented, best of %d)\n", repeat);
    std::printf("  %.2f ns/msg   %.2f cycles/msg   %.2f M msg/s\n",
                r.throughput_ns_per_msg,
                static_cast<double>(r.wall_cycles) / static_cast<double>(feed.msgs.size()),
                1000.0 / r.throughput_ns_per_msg);

    if (json_path != nullptr) {
        std::FILE* f = std::fopen(json_path, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", json_path);
            return 1;
        }
        std::fprintf(f, "{\n  \"messages\": %zu,\n  \"cycles_per_ns\": %.6f,\n",
                     feed.msgs.size(), cycles_per_ns);
        std::fprintf(f, "  \"refs_bits\": %d,\n", refs_bits);
        std::fprintf(f, "  \"harness_overhead_cycles\": %u,\n", overhead);
        std::fprintf(f, "  \"ns_per_msg\": %.4f,\n  \"cycles_per_msg\": %.4f,\n",
                     r.throughput_ns_per_msg,
                     static_cast<double>(r.wall_cycles) / static_cast<double>(feed.msgs.size()));
        std::fprintf(f, "  \"overall\": {\"p50\": %u, \"p99\": %u, \"p999\": %u, \"max\": %u},\n",
                     r.overall.percentile(50), r.overall.percentile(99),
                     r.overall.percentile(99.9), r.overall.max());
        std::fprintf(f, "  \"per_type\": {");
        bool first = true;
        for (size_t i = 0; i < kNumTypes; ++i) {
            const Histogram& h = r.per_type[i];
            if (h.empty()) continue;
            std::fprintf(f, "%s\n    \"%c\": {\"count\": %zu, \"p50\": %u, \"p99\": %u, \"p999\": %u}",
                         first ? "" : ",", kTypes[i], h.count(), h.percentile(50),
                         h.percentile(99), h.percentile(99.9));
            first = false;
        }
        std::fprintf(f, "\n  }\n}\n");
        std::fclose(f);
        std::printf("\nwrote %s\n", json_path);
    }
    return 0;
}
