#pragma once
//
// inventory_strategies.hpp — the baselines A-S is measured against.
//
// A comparison is only as good as what it controls for, and "A-S versus a
// touch-maker" controls for almost nothing: the two differ in inventory
// awareness AND in where they quote AND in how often they re-quote. An
// improvement could come from any of the three, and the phase 11.3 question --
// does inventory-aware quoting help, and through WHICH MECHANISM -- would be
// unanswerable.
//
// So there are two baselines, and they bracket the question.
//
//   SymmetricTouchMaker is the plan's baseline and phase 6's strategy: quote at
//   the touch, both sides, ignore inventory. It is what a naive maker does, and
//   comparing against it answers "is A-S better than the obvious thing".
//
//   AsMaker with gamma = 0 is the CONTROL. Same code, same spread formula, same
//   re-quote discipline, same quote size -- with the inventory skew turned off
//   and nothing else changed. Comparing against it answers the narrower and more
//   honest question: does the INVENTORY SKEW do anything, holding the rest of
//   the strategy fixed.
//
// The two together separate "A-S beats a naive maker" from "the skew is what
// beat it", and those are different claims that are routinely reported as one.
//
#include <cstdint>

#include "itchbook/sim/inventory_strategy.hpp"
#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

// Phase 6's touch-maker, under the closed-loop protocol.
//
// Deliberately inventory-blind: it takes an InventoryView and ignores the
// position field. That is what makes it the naive baseline rather than a
// crippled A-S.
struct SymmetricTouchMaker {
    uint32_t quote_size = 100;
    // Matched to AsConfig::requote_ticks so the two strategies re-quote on the
    // same discipline. Without this the comparison would be partly about
    // message rate, which is a different experiment.
    int32_t requote_ticks = 1;
    int32_t tick = 100;

    static const char* name() { return "symmetric-touch"; }

    void on_event(const InventoryView& v, Ctx& ctx) {
        int32_t b = 0;
        int32_t a = 0;
        if (!v.tradable || !v.best_bid(&b) || !v.best_ask(&a)) return;
        const int32_t threshold = requote_ticks * tick;
        if (quoted_ && std::abs(b - bid_) < threshold && std::abs(a - ask_) < threshold) {
            return;
        }
        if (quoted_) {
            ctx.cancel(bid_id_);
            ctx.cancel(ask_id_);
            ++requotes_;
        }
        bid_id_ = next_id_++;
        ask_id_ = next_id_++;
        ctx.quote(bid_id_, Side::Buy, b, quote_size);
        ctx.quote(ask_id_, Side::Sell, a, quote_size);
        bid_ = b;
        ask_ = a;
        quoted_ = true;
        quotes_ += 2;
    }

    uint64_t quotes() const { return quotes_; }
    uint64_t requotes() const { return requotes_; }

private:
    uint64_t next_id_ = 1;
    uint64_t bid_id_ = 0;
    uint64_t ask_id_ = 0;
    int32_t bid_ = 0;
    int32_t ask_ = 0;
    bool quoted_ = false;
    uint64_t quotes_ = 0;
    uint64_t requotes_ = 0;
};

// Inventory over time, which is the axis phase 11.3 predicts A-S wins on.
//
// TIME-WEIGHTED, not per-event. A position held for an hour and a position held
// for a microsecond are not the same risk, and an event-weighted mean would
// count them equally -- which on a busy symbol means the mean is dominated by
// whatever the inventory happened to be during the busiest minute.
class InventoryPath {
public:
    void observe(uint64_t ts, int64_t position) {
        if (!started_) {
            started_ = true;
            last_ts_ = ts;
            last_pos_ = position;
            return;
        }
        const uint64_t dt = ts > last_ts_ ? ts - last_ts_ : 0;
        if (dt > 0) {
            const double seconds = static_cast<double>(dt) / 1e9;
            const double p = static_cast<double>(last_pos_);
            total_seconds_ += seconds;
            sum_ += p * seconds;
            sum_sq_ += p * p * seconds;
            const int64_t abs_p = last_pos_ < 0 ? -last_pos_ : last_pos_;
            if (abs_p > max_abs_) max_abs_ = abs_p;
            if (last_pos_ > max_long_) max_long_ = last_pos_;
            if (last_pos_ < max_short_) max_short_ = last_pos_;
        }
        last_ts_ = ts;
        last_pos_ = position;
    }

    double mean() const { return total_seconds_ > 0 ? sum_ / total_seconds_ : 0.0; }
    double variance() const {
        if (total_seconds_ <= 0) return 0.0;
        const double m = mean();
        const double v = sum_sq_ / total_seconds_ - m * m;
        return v > 0.0 ? v : 0.0;
    }
    int64_t max_abs() const { return max_abs_; }
    int64_t max_long() const { return max_long_; }
    int64_t max_short() const { return max_short_; }
    double seconds() const { return total_seconds_; }

private:
    bool started_ = false;
    uint64_t last_ts_ = 0;
    int64_t last_pos_ = 0;
    double total_seconds_ = 0.0;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
    int64_t max_abs_ = 0;
    int64_t max_long_ = 0;
    int64_t max_short_ = 0;
};

}  // namespace itchbook::sim
