#pragma once
//
// backtest.hpp — one feed, one strategy, four fill models, one pass.
//
// All four lanes share a single book::Book. That is sound only because our
// orders never enter it: the moment market impact is modelled, the four worlds
// fork and each needs its own book. Because they share it, "same data" is a
// structural fact rather than a claim, and the containment invariants can be
// checked per event instead of once on end-of-day totals.
//
// The strategy is driven ONCE per event and its intents are broadcast to every
// lane. Since it cannot see fills or position, its decisions cannot diverge
// between lanes — so the only difference between the four P&L numbers is the
// fill model.
//
#include <algorithm>
#include <cstdint>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/sim/ledger.hpp"
#include "itchbook/sim/markout.hpp"
#include "itchbook/sim/queue_model.hpp"
#include "itchbook/sim/resolve.hpp"
#include "itchbook/sim/strategy.hpp"

namespace itchbook::sim {

struct LaneResult {
    Model model = Model::Naive;
    uint64_t fills = 0;
    uint64_t shares = 0;
    Money equity = 0;
    Money edge = 0;
    Money drift = 0;
    Money fees = 0;
    Money equity_per_share = 0;
    int64_t residual_position = 0;
    uint64_t clamp_events = 0;
    uint64_t priority_anomalies = 0;
    uint64_t lock_fills = 0;
    uint64_t through_fills = 0;
    HorizonReport markouts[kNumHorizons];
};

struct Lane {
    Model model;
    QueueModel queue;
    Ledger ledger;
    MarkoutEngine markout;
    std::vector<SimFill> pending;
    // Counted separately because a run dominated by price-priority fills is a
    // run in which the queue assumption is not what is being measured, and the
    // reader has to be able to see that.
    uint64_t lock_fills = 0;
    uint64_t through_fills = 0;

    Lane(Model m, QueueConfig c, FeeSchedule f) : model(m), queue(c), ledger(f) {}
};

template <typename Strategy>
class Backtest {
public:
    Backtest(Strategy strategy, FeeSchedule fees)
        : strategy_(strategy) {
        for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
            QueueConfig c;
            c.model = m;
            lanes_.emplace_back(m, c, fees);
        }
    }

    void on_message(char type, const uint8_t* p, uint16_t) {
        // 1. Session and trading state first, so every lane sees identical
        //    eligibility. Only 'T' trades: 'H', 'P' and 'Q' (quotation only,
        //    the state a symbol sits in while a halt-resumption cross runs) all
        //    do not, and '\0' is unknown-and-not-tradable.
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
        for (Lane& l : lanes_) l.queue.set_trading_state(tradable ? 'T' : trading_state_);

        // 2 + 3. Snapshot before the mutation, because E/C/X/D/U carry only a
        //        reference and the order's side, price and size vanish with it.
        book::PreState pre;
        book::apply_ex(book_, type, p, &pre);

        const uint64_t ts = itch::timestamp(p);
        const Mid mid = observe(book_, in_continuous_, tradable);
        if (mid.ok()) last_good_mid_ = mid;

        // 4. Resolve once, commit to every lane.
        const Resolved r = resolve(type, p, pre);
        for (Lane& l : lanes_) {
            l.pending.clear();
            l.queue.commit(r, book_, &l.pending);
            l.queue.apply_price_priority(book_, ts, &l.pending);
            for (const SimFill& f : l.pending) {
                l.ledger.on_fill(f, mid);
                l.markout.on_fill(f, mid);
                if (f.trigger == Trigger::Lock) ++l.lock_fills;
                if (f.trigger == Trigger::Through) ++l.through_fills;
            }
            l.markout.observe(ts, mid);
        }

        // 5. The strategy sees the market once and its intents go to every lane.
        ctx_.clear();
        MarketView view;
        view.book = &book_;
        view.mid = mid;
        view.ts = ts;
        view.tradable = tradable;
        view.event_index = event_index_;
        strategy_.on_event(view, ctx_);

        for (const Intent& in : ctx_.intents()) {
            for (Lane& l : lanes_) {
                switch (in.kind) {
                    case IntentKind::Quote:
                        l.queue.place(book_, in.id, in.side, in.price, in.quantity,
                                      in.display, ts);
                        break;
                    case IntentKind::Cancel:
                        l.queue.cancel(in.id);
                        break;
                    case IntentKind::Take: {
                        // Crossing the spread is not a queue question: we pay
                        // the touch and trade immediately. Capped at what is
                        // actually resting, so a taker cannot invent liquidity.
                        const uint64_t avail =
                            book_.shares_at(to_char(opposite(in.side)), in.price);
                        const uint32_t qty = static_cast<uint32_t>(
                            std::min<uint64_t>(in.quantity, avail));
                        if (qty == 0) break;
                        SimFill f;
                        f.order_id = in.id;
                        f.side = in.side;
                        f.price = in.price;
                        f.shares = qty;
                        f.ts = ts;
                        f.trigger = Trigger::Taking;
                        f.liquidity = Liquidity::Removed;
                        l.ledger.on_fill(f, mid);
                        l.markout.on_fill(f, mid);
                        break;
                    }
                }
            }
        }
        ++event_index_;
    }

    std::vector<LaneResult> results() const {
        // The residual is marked at the last usable CONTINUOUS-session mid,
        // never at the closing cross: the cross is a different auction at a
        // price the continuous book never had to honour, and marking against it
        // imports auction noise into every lane's headline number.
        const Price4 mark = last_good_mid_.ok()
                                ? static_cast<Price4>(last_good_mid_.two_mid / 2)
                                : 0;
        std::vector<LaneResult> out;
        for (const Lane& l : lanes_) {
            LaneResult r;
            r.model = l.model;
            r.fills = l.ledger.fills().size();
            r.shares = l.ledger.shares_traded();
            r.equity = l.ledger.equity(mark);
            r.edge = l.ledger.edge();
            r.drift = l.ledger.drift(last_good_mid_.two_mid);
            r.fees = l.ledger.fees();
            r.equity_per_share = l.ledger.per_share(r.equity);
            r.residual_position = l.ledger.position();
            r.clamp_events = l.queue.clamp_events();
            r.priority_anomalies = l.queue.priority_anomalies();
            r.lock_fills = l.lock_fills;
            r.through_fills = l.through_fills;
            for (size_t h = 0; h < kNumHorizons; ++h) r.markouts[h] = l.markout.report(h);
            out.push_back(r);
        }
        return out;
    }

    uint64_t events() const { return event_index_; }
    const book::Book& book() const { return book_; }
    Mid last_mid() const { return last_good_mid_; }

private:
    Strategy strategy_;
    Ctx ctx_;
    book::Book book_{100, 20, 1u << 20};
    std::vector<Lane> lanes_;
    uint64_t event_index_ = 0;
    char trading_state_ = '\0';
    bool in_continuous_ = false;
    Mid last_good_mid_;
};

}  // namespace itchbook::sim
