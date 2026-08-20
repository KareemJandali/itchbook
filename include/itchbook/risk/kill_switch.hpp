#pragma once
//
// kill_switch.hpp — the control that stops trading, and why it is not the
// control phase 6 already had.
//
// The backtester's `RiskLimits` refuses to SEND an order that would breach a
// position limit. That is a pre-trade check and it is necessary, but it is not
// sufficient, and treating it as sufficient is a classic way to be surprised:
//
//   * **Orders already resting still fill.** A pre-trade check runs when you
//     decide to quote. Between then and the fill, the quote sits in the book
//     with your name on it. Refuse every new order the moment you touch the
//     limit and you can still blow through it, because the fills that take you
//     there were committed before the check ever ran.
//   * **Latency widens the hole.** A cancel takes as long to arrive as an
//     order, so the window in which a limit is breachable is at minimum one
//     round trip wide, and it is exactly the window in which the market is
//     moving fast enough that you wanted out.
//   * **Some limits have no pre-trade form at all.** A drawdown limit is a
//     statement about realised losses. There is no order you can decline in
//     advance that makes it not have happened.
//
// So this is a POST-trade control, and it is latching. Once tripped it stays
// tripped until a human resets it. A risk control that re-arms itself when the
// condition clears is not a kill switch; it is a way to have the same incident
// several times in one afternoon.
//
// Each of the five limits is independently configurable and independently
// disabled by leaving it at zero. Nothing here is on by default: a limit the
// operator did not choose is a limit nobody has thought about, and a surprise
// halt in production is its own kind of outage.
//
#include <cstddef>
#include <cstdint>

#include "itchbook/sim/money.hpp"

namespace itchbook::risk {

using sim::Money;
using sim::Price4;

enum class Trip : uint8_t {
    None = 0,
    Position,      // absolute share position
    Notional,      // absolute dollar exposure
    MessageRate,   // our outbound messages per second
    OrderToFill,   // orders sent per fill received
    Drawdown,      // peak-to-trough equity
};

inline const char* to_string(Trip t) {
    switch (t) {
        case Trip::None:        return "none";
        case Trip::Position:    return "position";
        case Trip::Notional:    return "notional";
        case Trip::MessageRate: return "message-rate";
        case Trip::OrderToFill: return "order-to-fill";
        case Trip::Drawdown:    return "drawdown";
    }
    return "?";
}

struct KillSwitchConfig {
    // Absolute share position. Zero disables.
    int64_t max_position = 0;

    // Absolute exposure in micro-dollars: |position| * mark. Zero disables.
    // Shares alone are the wrong unit across symbols — 1,000 shares of a $4
    // stock and 1,000 of a $400 one are not the same risk — so both exist and
    // either can trip.
    Money max_notional = 0;

    // Our OUTBOUND messages in any one-second window. Zero disables. This is
    // the limit that catches a strategy stuck in a requote loop, which is the
    // most common way an automated system becomes a problem for the venue
    // before it becomes a problem for its own P&L.
    uint64_t max_messages_per_second = 0;

    // Orders sent per fill received, as an integer ratio. Zero disables.
    // Expressed as a ratio of counts rather than a float so the comparison is
    // exact: `orders > max * fills` never rounds.
    uint64_t max_orders_per_fill = 0;
    // ...but not before there is enough evidence to judge. The first order of
    // the day has zero fills behind it and would trip any ratio instantly, so
    // the ratio is not evaluated until this many orders have been sent.
    uint64_t ratio_min_orders = 100;

    // Peak-to-trough equity decline, in micro-dollars, as a positive number.
    // Zero disables. Measured against the running peak rather than the open,
    // because a strategy that made $10k and gave back $9k had a bad day even
    // though it is up on the session.
    Money max_drawdown = 0;
};

// What tripped, what the observed value was, and what it was measured against.
// The limit is carried alongside the observation because "position limit
// breached" in a log at 2am is not actionable and "position 1,240 against a
// limit of 1,000 at 14:03:11.4" is.
struct TripReport {
    Trip reason = Trip::None;
    int64_t observed = 0;
    int64_t limit = 0;
    uint64_t ts = 0;

    bool tripped() const { return reason != Trip::None; }
};

// A one-second sliding window in 100ms buckets.
//
// Ten buckets rather than one counter because a fixed one-second window resets
// on a boundary, and a strategy sending 2N messages across a boundary — N just
// before and N just after — never registers above N under a fixed window while
// genuinely having sent 2N in one second. The bucket a timestamp lands in is
// its own epoch, so a stale bucket is recognised as stale rather than
// zeroed on a timer.
class RateWindow {
public:
    static constexpr size_t kBuckets = 10;
    static constexpr uint64_t kBucketNs = 100000000ULL;   // 100ms

    void add(uint64_t ts) {
        const uint64_t idx = ts / kBucketNs;
        const size_t slot = idx % kBuckets;
        if (epoch_[slot] != idx) {
            epoch_[slot] = idx;
            count_[slot] = 0;
        }
        ++count_[slot];
        if (idx > now_ || now_ == kNever) now_ = idx;
    }

    uint64_t rate() const {
        if (now_ == kNever) return 0;
        uint64_t total = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            // Unsigned subtraction: a bucket never written holds kNever, so the
            // difference wraps to something enormous and is excluded. That is
            // deliberate, and it is why epochs start at kNever and not zero —
            // zero is a legitimate bucket index at the very start of a session.
            if (now_ - epoch_[i] < kBuckets) total += count_[i];
        }
        return total;
    }

    void reset() {
        for (size_t i = 0; i < kBuckets; ++i) {
            epoch_[i] = kNever;
            count_[i] = 0;
        }
        now_ = kNever;
    }

private:
    static constexpr uint64_t kNever = ~0ULL;
    uint64_t epoch_[kBuckets] = {kNever, kNever, kNever, kNever, kNever,
                                 kNever, kNever, kNever, kNever, kNever};
    uint64_t count_[kBuckets] = {};
    uint64_t now_ = kNever;
};

class KillSwitch {
public:
    KillSwitch() = default;
    explicit KillSwitch(KillSwitchConfig cfg) : cfg_(cfg) {}

    // ---- what the system tells the switch --------------------------------

    // One outbound message: a new order, a cancel, a replace. Counted for the
    // rate limit; orders (not cancels) are also counted for the ratio.
    void on_message_sent(uint64_t ts, bool is_order) {
        ++messages_;
        if (is_order) ++orders_;
        rate_.add(ts);
        check_rate(ts);
        check_ratio(ts);
    }

    // One fill. Position and equity come from the caller's ledger rather than
    // being recomputed here: there is exactly one place in this system that
    // knows what a fill is worth, and duplicating that arithmetic is how two
    // answers to the same question appear.
    void on_fill(uint64_t ts, int64_t position, Money equity, Price4 mark) {
        ++fills_;
        observe(ts, position, equity, mark);
    }

    // A mark update with no fill. Drawdown moves without trading, so a switch
    // that only looks on fills is blind exactly while a position is being
    // marked against a market running away from it.
    void observe(uint64_t ts, int64_t position, Money equity, Price4 mark) {
        if (equity > peak_equity_ || !peak_seen_) {
            peak_equity_ = equity;
            peak_seen_ = true;
        }
        last_equity_ = equity;
        check_position(ts, position);
        check_notional(ts, position, mark);
        check_drawdown(ts);
    }

    // ---- what the system asks the switch ---------------------------------

    bool live() const { return report_.reason == Trip::None; }
    const TripReport& report() const { return report_; }

    // Explicit, and deliberately not called by anything in the hot path. A
    // reset is an operator decision, not a recovery step.
    void reset() {
        report_ = TripReport{};
        rate_.reset();
        messages_ = 0;
        orders_ = 0;
        fills_ = 0;
        peak_seen_ = false;
        peak_equity_ = 0;
    }

    uint64_t messages() const { return messages_; }
    uint64_t orders() const { return orders_; }
    uint64_t fills() const { return fills_; }
    Money peak_equity() const { return peak_equity_; }
    Money drawdown() const { return peak_seen_ ? peak_equity_ - last_equity_ : 0; }
    uint64_t message_rate() const { return rate_.rate(); }

private:
    // The latch. The FIRST breach is the one recorded: everything after a trip
    // is a consequence of not having stopped, and overwriting the cause with
    // its own aftermath is how a post-mortem loses the thread.
    void trip(Trip reason, int64_t observed, int64_t limit, uint64_t ts) {
        if (report_.reason != Trip::None) return;
        report_ = TripReport{reason, observed, limit, ts};
    }

    void check_position(uint64_t ts, int64_t position) {
        if (cfg_.max_position <= 0) return;
        const int64_t abs_pos = position < 0 ? -position : position;
        if (abs_pos > cfg_.max_position) {
            trip(Trip::Position, abs_pos, cfg_.max_position, ts);
        }
    }

    void check_notional(uint64_t ts, int64_t position, Price4 mark) {
        if (cfg_.max_notional <= 0 || mark <= 0) return;
        const int64_t abs_pos = position < 0 ? -position : position;
        const Money exposure = sim::notional(mark, abs_pos);
        if (exposure > cfg_.max_notional) {
            trip(Trip::Notional, exposure, cfg_.max_notional, ts);
        }
    }

    void check_rate(uint64_t ts) {
        if (cfg_.max_messages_per_second == 0) return;
        const uint64_t r = rate_.rate();
        if (r > cfg_.max_messages_per_second) {
            trip(Trip::MessageRate, static_cast<int64_t>(r),
                 static_cast<int64_t>(cfg_.max_messages_per_second), ts);
        }
    }

    void check_ratio(uint64_t ts) {
        if (cfg_.max_orders_per_fill == 0) return;
        if (orders_ < cfg_.ratio_min_orders) return;
        // orders / fills > max, without the division. With no fills at all the
        // ratio is unbounded, so the min-orders gate above is what keeps this
        // from tripping on a quiet open.
        const bool over = fills_ == 0
                              ? orders_ > cfg_.max_orders_per_fill
                              : orders_ > cfg_.max_orders_per_fill * fills_;
        if (over) {
            trip(Trip::OrderToFill, static_cast<int64_t>(orders_),
                 static_cast<int64_t>(cfg_.max_orders_per_fill *
                                      (fills_ == 0 ? 1 : fills_)),
                 ts);
        }
    }

    void check_drawdown(uint64_t ts) {
        if (cfg_.max_drawdown <= 0 || !peak_seen_) return;
        const Money dd = peak_equity_ - last_equity_;
        if (dd > cfg_.max_drawdown) {
            trip(Trip::Drawdown, dd, cfg_.max_drawdown, ts);
        }
    }

    KillSwitchConfig cfg_;
    TripReport report_;
    RateWindow rate_;
    uint64_t messages_ = 0;
    uint64_t orders_ = 0;
    uint64_t fills_ = 0;
    bool peak_seen_ = false;
    Money peak_equity_ = 0;
    Money last_equity_ = 0;
};

}  // namespace itchbook::risk
