#pragma once
//
// closed_loop.hpp — one world, run to its own conclusion.
//
// backtest.hpp runs ONE strategy against FOUR fill models and grades the same
// intent stream four ways. That construction depends on the strategy not seeing
// its fills, and inventory_strategy.hpp explains at length why an
// inventory-aware strategy cannot live inside it: a lane that fills less carries
// less inventory, quotes differently, and fills differently again. The four
// lanes stop being four scores of one decision stream from the first fill
// onward.
//
// So this runs ONE lane with ONE strategy that sees its own fills, and the band
// is built by running it four times. Each run is a complete, internally
// consistent answer to "what would this strategy have done if fills worked like
// THIS" -- a band over worlds rather than over gradings.
//
// TWO DRIVERS THAT MUST NOT DRIFT, AND THE INVARIANT THAT HOLDS THEM TOGETHER.
//
// This file and backtest.hpp orchestrate the same components -- QueueModel,
// Ledger, MarkoutEngine, KillSwitch, LatencyModel, RiskLimits -- in the same
// order, and that similarity is a liability. The 10.6 lesson was that two copies
// of one piece of logic either disagree on a trailing zero or agree while both
// being wrong.
//
// A comment cannot prevent that, so there is a test instead:
// test_closed_loop.cpp requires that a strategy WITHOUT feedback produces
// results identical to the corresponding lane of backtest.hpp, field for field,
// on the same feed. A strategy that ignores its fills is the case where the two
// drivers are answering the same question, so they must answer it the same way.
// If either file's ordering drifts -- when actions arrive, whether the switch
// sees a mark before or after a fill, how the residual is marked -- that test
// fails and names the field.
//
// WHAT IS DELIBERATELY THE SAME AS PHASE 6, because these decisions were argued
// once and re-deciding them here would silently fork the methodology:
//
//   * Actions land only when the clock reaches them, applied AFTER the message
//     at that timestamp, so a same-nanosecond tie resolves against us.
//   * Queue position is computed from the book AT ARRIVAL, never at decision
//     time -- the order never met the decision-time book.
//   * The risk limit binds at arrival, on the position the order actually met.
//   * A tripped switch cancels everything resting; a limit only declines one
//     order.
//   * The residual is marked at the last continuous-session mid, never at the
//     closing cross.
//
#include <algorithm>
#include <cstdint>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/risk/kill_switch.hpp"
#include "itchbook/sim/backtest.hpp"
#include "itchbook/sim/inventory_strategy.hpp"

namespace itchbook::sim {

// The session horizon, which A-S needs as (T - t) and a replay cannot infer.
//
// CONFIGURATION, not state: it is the operator saying which session this is, so
// it is never restored from a snapshot. Defaults to the regular US equity
// session, in nanoseconds since midnight, which is the unit ITCH timestamps use.
struct SessionClock {
    uint64_t start_ns = 9ULL * 3600 * 1000000000ULL + 30ULL * 60 * 1000000000ULL;
    uint64_t end_ns = 16ULL * 3600 * 1000000000ULL;

    // Progress in [0, 1]. Clamped rather than extrapolated: a message before the
    // open or after the close is a real thing in an ITCH file, and letting it
    // produce a negative (T - t) would make A-S's spread term imaginary.
    double elapsed(uint64_t ts) const {
        if (end_ns <= start_ns || ts <= start_ns) return 0.0;
        if (ts >= end_ns) return 1.0;
        return static_cast<double>(ts - start_ns) /
               static_cast<double>(end_ns - start_ns);
    }
};

template <typename Strategy>
class ClosedLoopBacktest {
public:
    ClosedLoopBacktest(Strategy strategy, Model model, FeeSchedule fees,
                       LatencyModel latency = {}, RiskLimits risk = {},
                       risk::KillSwitchConfig kill = {}, SessionClock clock = {})
        : strategy_(strategy), model_(model), latency_(latency), risk_(risk),
          clock_(clock), queue_(make_config(model)), ledger_(fees), kill_(kill) {}

    void on_message(char type, const uint8_t* p, uint16_t) {
        if (type == 'H') {
            trading_state_ = itch::trading_action::state(p);
        } else if (type == 'S') {
            const char code = itch::system_event::code(p);
            if (code == 'Q') {
                in_continuous_ = true;
                trading_state_ = 'T';
            } else if (code == 'M' || code == 'C') {
                in_continuous_ = false;
            }
        }
        const bool tradable = in_continuous_ && trading_state_ == 'T';
        queue_.set_trading_state(tradable ? 'T' : trading_state_);

        book::PreState pre;
        book::apply_ex(book_, type, p, &pre);

        const uint64_t ts = itch::timestamp(p);
        const Mid mid = observe(book_, in_continuous_, tradable);
        if (mid.ok()) last_good_mid_ = mid;

        const Resolved r = resolve(type, p, pre);
        pending_fills_.clear();
        queue_.commit(r, book_, &pending_fills_);
        queue_.apply_price_priority(book_, ts, &pending_fills_);
        for (const SimFill& f : pending_fills_) {
            ledger_.on_fill(f, mid);
            markout_.on_fill(f, mid);
            if (f.trigger == Trigger::Lock) ++lock_fills_;
            if (f.trigger == Trigger::Through) ++through_fills_;
            const Price4 mk = mid.ok() ? static_cast<Price4>(mid.two_mid / 2) : 0;
            kill_.on_fill(f.ts, ledger_.position(), ledger_.equity(mk), mk);
            // THE FEEDBACK PATH. Delivered here, before the strategy sees the
            // market below, so a fill on this message is known to the decision
            // this message provokes. Delivering it after would give the strategy
            // a view of the world one event stale in exactly the moments that
            // matter most.
            deliver(f);
        }
        markout_.observe(ts, mid);
        if (mid.ok()) {
            kill_.observe(ts, ledger_.position(),
                          ledger_.equity(static_cast<Price4>(mid.two_mid / 2)),
                          static_cast<Price4>(mid.two_mid / 2));
        }
        if (!kill_.live() && !trip_seen_) {
            trip_seen_ = true;
            tripped_at_event_ = event_index_;
            queue_.cancel_all();
        }
        const int64_t pos = ledger_.position();
        if (pos > peak_position_ || -pos > peak_position_) {
            peak_position_ = pos < 0 ? -pos : pos;
        }

        ctx_.clear();
        InventoryView view;
        view.book = &book_;
        view.mid = mid;
        view.ts = ts;
        view.tradable = tradable;
        view.event_index = event_index_;
        // The harness's position, not one the strategy kept for itself. There is
        // one correct answer to "what is my position" and a strategy that
        // tracked it separately could be wrong in a way nothing would catch.
        view.position = tracker_.position();
        view.session_elapsed = clock_.elapsed(ts);
        strategy_.on_event(view, ctx_);

        for (const Intent& in : ctx_.intents()) {
            const uint64_t arrival = in.kind == IntentKind::Cancel
                                         ? latency_.arrival_for_cancel(ts)
                                         : latency_.arrival_for_quote(ts);
            pending_.push_back(PendingAction{arrival, action_seq_++, in});
        }
        apply_arrived(ts);
        ++event_index_;
    }

    void apply_arrived(uint64_t now) {
        size_t write = 0;
        for (size_t i = 0; i < pending_.size(); ++i) {
            const PendingAction& pa = pending_[i];
            if (pa.arrival_ns > now) {
                pending_[write++] = pa;
                continue;
            }
            apply_intent(pa.intent, pa.arrival_ns);
        }
        pending_.resize(write);
    }

    LaneResult result() const {
        const Price4 mark = last_good_mid_.ok()
                                ? static_cast<Price4>(last_good_mid_.two_mid / 2)
                                : 0;
        LaneResult r;
        r.model = model_;
        r.fills = ledger_.fills().size();
        r.shares = ledger_.shares_traded();
        r.equity = ledger_.equity(mark);
        r.edge = ledger_.edge();
        r.drift = ledger_.drift(last_good_mid_.two_mid);
        r.fees = ledger_.fees();
        r.equity_per_share = ledger_.per_share(r.equity);
        r.residual_position = ledger_.position();
        r.clamp_events = queue_.clamp_events();
        r.priority_anomalies = queue_.priority_anomalies();
        r.lock_fills = lock_fills_;
        r.through_fills = through_fills_;
        r.suppressed_quotes = suppressed_quotes_;
        r.peak_position = peak_position_;
        r.trip = kill_.report().reason;
        r.tripped_at_event = tripped_at_event_;
        r.trip_observed = kill_.report().observed;
        r.trip_limit = kill_.report().limit;
        for (size_t h = 0; h < kNumHorizons; ++h) r.markouts[h] = markout_.report(h);
        return r;
    }

    uint64_t events() const { return event_index_; }
    const book::Book& book() const { return book_; }
    Mid last_mid() const { return last_good_mid_; }
    int64_t position() const { return tracker_.position(); }
    const Strategy& strategy() const { return strategy_; }

private:
    static QueueConfig make_config(Model m) {
        QueueConfig c;
        c.model = m;
        return c;
    }

    // on_fill is optional. A strategy that does not want its fills still gets a
    // correct `position` from the tracker, so omitting it is a real choice
    // rather than a broken one.
    void deliver(const SimFill& f) {
        const FillEvent e = FillEvent::from(f);
        tracker_.on_fill(e);
        if constexpr (requires(Strategy& s, const FillEvent& ev) { s.on_fill(ev); }) {
            strategy_.on_fill(e);
        }
    }

    void apply_intent(const Intent& in, uint64_t arrival_ts) {
        const Mid mid = last_good_mid_;
        if (!kill_.live()) {
            ++suppressed_quotes_;
            return;
        }
        kill_.on_message_sent(arrival_ts, in.kind != IntentKind::Cancel);
        switch (in.kind) {
            case IntentKind::Quote:
                if (risk_.blocks(ledger_.position(), in.side, in.quantity)) {
                    ++suppressed_quotes_;
                    break;
                }
                queue_.place(book_, in.id, in.side, in.price, in.quantity,
                             in.display, arrival_ts);
                break;
            case IntentKind::Cancel:
                queue_.cancel(in.id);
                break;
            case IntentKind::Take: {
                if (risk_.blocks(ledger_.position(), in.side, in.quantity)) {
                    ++suppressed_quotes_;
                    break;
                }
                const uint64_t avail =
                    book_.shares_at(to_char(opposite(in.side)), in.price);
                const uint32_t qty =
                    static_cast<uint32_t>(std::min<uint64_t>(in.quantity, avail));
                if (qty == 0) break;
                SimFill f;
                f.order_id = in.id;
                f.side = in.side;
                f.price = in.price;
                f.shares = qty;
                f.ts = arrival_ts;
                f.trigger = Trigger::Taking;
                f.liquidity = Liquidity::Removed;
                ledger_.on_fill(f, mid);
                markout_.on_fill(f, mid);
                {
                    const Price4 mk = mid.ok() ? static_cast<Price4>(mid.two_mid / 2) : 0;
                    kill_.on_fill(f.ts, ledger_.position(), ledger_.equity(mk), mk);
                }
                deliver(f);
                break;
            }
        }
    }

    Strategy strategy_;
    Model model_;
    LatencyModel latency_;
    RiskLimits risk_;
    SessionClock clock_;

    book::Book book_;
    QueueModel queue_;
    Ledger ledger_;
    MarkoutEngine markout_;
    risk::KillSwitch kill_;
    PositionTracker tracker_;
    Ctx ctx_;

    std::vector<SimFill> pending_fills_;
    std::vector<PendingAction> pending_;
    uint64_t action_seq_ = 0;
    uint64_t event_index_ = 0;
    uint64_t lock_fills_ = 0;
    uint64_t through_fills_ = 0;
    uint64_t suppressed_quotes_ = 0;
    int64_t peak_position_ = 0;
    uint64_t tripped_at_event_ = 0;
    bool trip_seen_ = false;
    Mid last_good_mid_;
    char trading_state_ = '\0';
    bool in_continuous_ = false;
};

}  // namespace itchbook::sim
