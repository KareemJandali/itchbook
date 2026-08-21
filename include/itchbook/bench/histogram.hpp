#pragma once
//
// histogram.hpp — latency samples and the percentiles that matter.
//
// p50 is what the machine usually does. p99 and p99.9 are what it does when a
// cache misses, a level allocates, or the ref map probes long — which is the
// behaviour that actually matters in a trading system, and the reason a mean
// is close to useless here.
//
// Samples are stored raw rather than bucketed: a full trading day for one
// symbol is a few million messages, so at 4 bytes each the whole thing fits in
// a few tens of MB and the percentiles stay exact.
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace itchbook::bench {

class Histogram {
public:
    // Reserve up front. Growing mid-run would allocate inside the measured
    // region, which is the one thing the harness must never do.
    explicit Histogram(size_t expected) { samples_.reserve(expected); }

    // Saturates rather than wrapping: a 4-billion-cycle message means the
    // process was descheduled, and clamping keeps one such sample from
    // swamping the tail.
    void add(uint64_t cycles) {
        samples_.push_back(cycles > UINT32_MAX ? UINT32_MAX
                                               : static_cast<uint32_t>(cycles));
    }

    size_t count() const { return samples_.size(); }
    bool empty() const { return samples_.empty(); }

    // Sorts in place — call once, after the measured region.
    void finalize() {
        std::sort(samples_.begin(), samples_.end());
        sorted_ = true;
    }

    uint32_t percentile(double p) const {
        if (samples_.empty()) return 0;
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(samples_.size()));
        if (idx >= samples_.size()) idx = samples_.size() - 1;
        return samples_[idx];
    }

    uint32_t min() const { return samples_.empty() ? 0 : samples_.front(); }
    uint32_t max() const { return samples_.empty() ? 0 : samples_.back(); }

    double mean() const {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (uint32_t s : samples_) sum += s;
        return sum / static_cast<double>(samples_.size());
    }

    uint64_t total() const {
        uint64_t sum = 0;
        for (uint32_t s : samples_) sum += s;
        return sum;
    }

    bool sorted() const { return sorted_; }

    // ---- the distribution itself, not just five points on it ----------------
    //
    // Percentiles say where the tail is; they do not say what it looks like.
    // Two builds can report the same p50 and p99 and have completely different
    // shapes — one tight cluster with a few stragglers, or two separate modes
    // because a branch is taken half the time. The second is a mechanism you
    // can go and find; the percentile table hides it.
    //
    // Buckets are logarithmic, four per octave. Cycle latencies here span from
    // tens to tens of millions, so a linear histogram is a single bar on the
    // left and six empty screens to the right of it. Four per octave is fine
    // enough to separate a 60-cycle mode from a 90-cycle one and coarse enough
    // that the whole range fits in about a hundred rows.
    struct Bucket {
        uint64_t lo;      // inclusive
        uint64_t hi;      // exclusive
        uint64_t count;
    };

    std::vector<Bucket> buckets() const {
        std::vector<Bucket> out;
        if (samples_.empty()) return out;
        // Edges: 0, then 1,2,3,4,5,6,8,10,13,16,... rounded up so no two
        // collide at the bottom of the range where the spacing is sub-integer.
        std::vector<uint64_t> edges{0, 1};
        double v = 1.0;
        const double step = 1.189207115002721;   // 2^(1/4)
        while (edges.back() <= UINT32_MAX) {
            v *= step;
            auto e = static_cast<uint64_t>(v + 0.5);
            if (e > edges.back()) edges.push_back(e);
        }
        out.reserve(edges.size());
        for (size_t i = 0; i + 1 < edges.size(); ++i) out.push_back({edges[i], edges[i + 1], 0});
        for (uint32_t sample : samples_) {
            // Linear scan would be O(n * buckets); binary search keeps this
            // proportional to the sample count, which is millions.
            size_t lo = 0, hi = out.size();
            while (lo + 1 < hi) {
                size_t mid = (lo + hi) / 2;
                if (out[mid].lo <= sample) lo = mid; else hi = mid;
            }
            ++out[lo].count;
        }
        return out;
    }

    // lo,hi,count — one row per non-empty bucket. Empty buckets are dropped
    // rather than written as zeros: on a log axis the gap is the information,
    // and the renderer places bars by their bounds, not by row index.
    bool write_buckets_csv(const char* path) const {
        std::FILE* f = std::fopen(path, "w");
        if (f == nullptr) return false;
        std::fprintf(f, "lo,hi,count\n");
        for (const Bucket& b : buckets()) {
            if (b.count == 0) continue;
            std::fprintf(f, "%llu,%llu,%llu\n",
                         static_cast<unsigned long long>(b.lo),
                         static_cast<unsigned long long>(b.hi),
                         static_cast<unsigned long long>(b.count));
        }
        const bool bad = std::ferror(f) != 0;
        return std::fclose(f) == 0 && !bad;
    }

private:
    std::vector<uint32_t> samples_;
    bool sorted_ = false;
};

}  // namespace itchbook::bench
