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

private:
    std::vector<uint32_t> samples_;
    bool sorted_ = false;
};

}  // namespace itchbook::bench
