#pragma once
//
// queue_model.hpp — where our passive order sits, and when it would have filled.
//
// This is the phase. When you rest an order at price P you are behind whatever
// is already there, and you only trade once that much has gone. Executions
// unambiguously advance you: price-time priority takes them from the front. A
// cancel does not, because the feed tells you shares left the level and not
// whether they were in front of you.
//
// Four models, differing in ONE boolean — was this cancel ahead of us?
//
//   naive        no queue at all; fill on any printable trade at-or-through us
//   optimistic   every cancel was ahead of us
//   pessimistic  no cancel was ahead of us; only executions advance
//   mbo          resolved by order reference (see below)
//
// The fourth exists because the honest answer to "you cannot know" is: on THIS
// feed you often can. TotalView-ITCH is order-by-order, so every cancel names a
// reference, and whether that reference was resting here when we arrived is a
// fact we recorded. The bracket is what an AGGREGATED feed forces on you; `mbo`
// is what this one actually supports, and it doubles as an internal oracle —
// the bounds must contain it, per order, per event.
//
// It is `mbo` and not `exact`: it is exact with respect to displayed liquidity
// on one venue, and blind to hidden interest and display-price sliding.
//
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/sim/event.hpp"

namespace itchbook::sim {

enum class Model : uint8_t { Naive, Optimistic, Pessimistic, Mbo };

inline const char* to_string(Model m) {
    switch (m) {
        case Model::Naive:       return "naive";
        case Model::Optimistic:  return "optimistic";
        case Model::Pessimistic: return "pessimistic";
        case Model::Mbo:         return "mbo";
    }
    return "?";
}

struct QueueConfig {
    Model model = Model::Pessimistic;
    // The clamp bounds `ahead` by what is actually resting at our price. Without
    // it, pessimistic can claim thousands of shares sit in front of us at a
    // provably empty level, which is not a bound but a bug. With it,
    // "pessimistic" means "you advance only as fast as executions and the
    // level's shallowest moment allow" — a real change of meaning that the
    // write-up has to state, which is why it is switchable and why the clamp
    // counters are reported.
    bool clamp = true;
    // Whether the naive model fills on hidden ('P') prints. A naive backtester
    // working from a trade tape does, so this is the faithful strawman — but it
    // inflates the naive curve, which is the direction that flatters this
    // project's headline. On by default, always reported separately.
    bool naive_fills_hidden = true;
};

// One resting order of ours, under one model.
struct Entry {
    uint64_t id = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t display = 0;      // our currently visible slice
    uint32_t slice_size = 0;   // the configured iceberg slice; == quantity if not one
    uint32_t hidden = 0;       // iceberg reserve, in no queue
    uint64_t ahead = 0;        // THE state variable: displayed shares in front
    uint64_t ahead0 = 0;       // ahead at arrival, for reporting
    uint64_t arrived_ns = 0;   // arrival, never the decision time
    // How many times this order's visible slice has been refreshed. The only
    // event that can legally INCREASE `ahead` — hiding size costs priority, so
    // a refreshed slice rejoins the back — which makes this the hook a property
    // test needs to tell a legal increase from a bug.
    uint32_t refreshes = 0;
    bool live = true;

    uint32_t remaining() const { return display + hidden; }
};

// For the `mbo` model: which references were resting ahead of us at arrival, and
// how many shares each still has. Membership must be O(1) — a linear scan
// reintroduces exactly the cost level.hpp warns about for shares_ahead_of().
using AheadSet = std::unordered_map<uint64_t, uint32_t>;

class QueueModel {
public:
    explicit QueueModel(QueueConfig cfg = {}) : cfg_(cfg) {}

    const QueueConfig& config() const { return cfg_; }
    Model model() const { return cfg_.model; }

    // ---- placing and removing our own orders --------------------------------

    // `book` must be the CURRENT book at the order's ARRIVAL time. Queue position
    // is a property of when the order reached the exchange, not when the strategy
    // decided to send it; using the decision-time book is look-ahead.
    void place(const book::Book& book, uint64_t id, Side side, Price4 price,
               uint32_t quantity, uint32_t display_size, uint64_t arrival_ns) {
        Entry e;
        e.id = id;
        e.side = side;
        e.price = price;
        e.display = (display_size > 0 && display_size < quantity) ? display_size : quantity;
        e.slice_size = e.display;
        e.hidden = quantity - e.display;
        e.arrived_ns = arrival_ns;
        e.ahead = book.shares_at(to_char(side), price) + own_live_ahead(side, price, arrival_ns);
        e.ahead0 = e.ahead;
        entries_.push_back(e);

        if (cfg_.model == Model::Mbo) {
            AheadSet set;
            for (const book::Order* o = book.first_order(to_char(side), price);
                 o != nullptr; o = o->next) {
                set.emplace(o->ref, o->shares);
            }
            ahead_sets_.emplace(id, std::move(set));
        }
    }

    void cancel(uint64_t id) {
        for (Entry& e : entries_) {
            if (e.id == id && e.live) {
                e.live = false;
                e.display = 0;
                e.hidden = 0;
            }
        }
        ahead_sets_.erase(id);
    }

    // ---- applying a resolved feed message -----------------------------------

    // `post` must be the book AFTER the message has been applied: the clamp needs
    // the level's new size. `r` was resolved BEFORE it, because the message's own
    // side and price no longer exist afterwards.
    void commit(const Resolved& r, const book::Book& post, std::vector<SimFill>* fills) {
        if (!tradable_) return;

        for (Entry& e : entries_) {
            if (!e.live || e.display == 0) continue;

            if (cfg_.model == Model::Naive) {
                naive_commit(e, r, fills, &post);
            } else {
                queued_commit(e, r, fills, &post);
            }
        }
        if (cfg_.clamp) clamp_all(post);
        reap();
    }

    // Price priority: the other side came to us. Applied identically in all four
    // models, because there is nothing to model — a resting order at our price is
    // taken out by an incoming order that locks or crosses it, and no queue
    // ambiguity applies. Omitting this deletes the single most important passive
    // fill there is, and every fill it deletes is an adverse-selection fill, so
    // the bias runs one way and flatters the result.
    void apply_price_priority(const book::Book& post, uint64_t ts,
                              std::vector<SimFill>* fills) {
        if (!tradable_) return;
        for (Entry& e : entries_) {
            if (!e.live || e.display == 0) continue;
            const char other = to_char(opposite(e.side));
            int32_t best = 0;
            if (!(e.side == Side::Buy ? post.best_ask(&best) : post.best_bid(&best))) continue;
            if (!at_or_through(e.side, e.price, best)) continue;

            // Everything on the other side at or through our price would have
            // traded with us first.
            std::vector<book::LevelView> levels;
            post.top(other, 32, &levels);
            uint64_t reach = 0;
            for (const auto& lv : levels) {
                if (!at_or_through(e.side, e.price, lv.price)) break;
                reach += lv.shares;
            }
            if (reach == 0) continue;
            trade_class(e, reach, ts, Trigger::Lock, fills, &post);
        }
        reap();
    }

    // ---- session gating -----------------------------------------------------
    //
    // Hoisted above the model dispatch so all four see byte-identical
    // eligibility. Only 'T' is tradable: 'H' halted, 'P' paused, and 'Q'
    // quotation-only (the state a symbol sits in while a halt-resumption cross
    // runs) are all not. '\0' is unknown-and-not-tradable, counted separately
    // and never treated as trading.
    void set_trading_state(char state) {
        state_ = state;
        tradable_ = (state == 'T');
    }
    char trading_state() const { return state_; }
    bool tradable() const { return tradable_; }

    // ---- queries ------------------------------------------------------------

    const std::vector<Entry>& entries() const { return entries_; }

    const Entry* find(uint64_t id) const {
        for (const Entry& e : entries_) {
            if (e.id == id) return &e;
        }
        return nullptr;
    }

    uint64_t clamp_events() const { return clamp_events_; }
    uint64_t clamp_shares() const { return clamp_shares_; }
    uint64_t priority_anomalies() const { return priority_anomalies_; }
    uint64_t anomaly_shares() const { return anomaly_shares_; }
    uint64_t hidden_inside_shares() const { return hidden_inside_shares_; }
    uint64_t naive_hidden_fills() const { return naive_hidden_fills_; }
    uint64_t naive_hidden_shares() const { return naive_hidden_shares_; }

private:
    // ---- the two arithmetic rules ------------------------------------------

    // TRADE-CLASS: consume the queue, then spill into a fill. `ahead` is read
    // BEFORE the decrement, and every subtraction compares before subtracting —
    // `removed - ahead` the other way round wraps to ~4e9 and reports a fill of
    // the entire order.
    void trade_class(Entry& e, uint64_t removed, uint64_t ts, Trigger trigger,
                     std::vector<SimFill>* fills, const book::Book* post) {
        const uint64_t used = std::min(removed, e.ahead);
        e.ahead -= used;
        const uint64_t overflow = removed - used;
        if (overflow == 0) return;

        // Capped at the DISPLAYED slice, never at display + hidden: an iceberg's
        // reserve is not in the queue and cannot be hit.
        const uint32_t fill = static_cast<uint32_t>(std::min<uint64_t>(overflow, e.display));
        if (fill == 0) return;

        e.display -= fill;
        e.ahead = 0;
        if (fills != nullptr) {
            fills->push_back(SimFill{e.id, e.side, e.price, fill, ts, trigger,
                                     Liquidity::Added, e.ahead0});
        }
        // A partial fill leaves us at the FRONT. Never re-derive `ahead` from the
        // book here: the book still holds real orders our counterfactual just
        // traded through, so re-deriving would re-insert us behind liquidity we
        // already consumed and make a partial fill strictly worse than none.
        if (e.display == 0 && e.hidden > 0) refresh_iceberg(e, post);
    }

    // CANCEL-CLASS: consume and discard. Never spills into a fill.
    static void cancel_class(Entry& e, uint64_t removed, bool from_ahead) {
        if (!from_ahead) return;
        e.ahead -= std::min(removed, e.ahead);
    }

    // An iceberg's next slice joins the BACK of the queue. Hiding size costs
    // priority; this is where that becomes a number.
    void refresh_iceberg(Entry& e, const book::Book* post) {
        const uint32_t slice = std::min<uint32_t>(e.hidden, e.slice_size);
        e.hidden -= slice;
        e.display = slice;
        ++e.refreshes;
        // Straight to the back of the queue as it stands right now. Reading the
        // post-mutation book matters: the shares this fill just consumed are
        // already gone from it, so we rejoin behind what is genuinely left.
        e.ahead = post == nullptr
                      ? 0
                      : post->shares_at(to_char(e.side), e.price) +
                            own_live_ahead(e.side, e.price, e.arrived_ns);
    }

    // ---- per-model commit ---------------------------------------------------

    void queued_commit(Entry& e, const Resolved& r, std::vector<SimFill>* fills,
                       const book::Book* post) {
        // R3 — price priority. A printable trade strictly through our limit
        // means a displayed order at our price would have traded first.
        //
        // Two conditions beyond the price test:
        //
        //   * Side is checked only when the message actually carries a reliable
        //     one. A 'P' does not: its Buy/Sell field is the side of the
        //     non-displayed order and is widely observed to be constant. The
        //     price test alone is sound anyway — a print through our limit
        //     should have hit us first whichever side rested.
        //
        //   * Suppressed while the other side is already at-or-through us, i.e.
        //     while R1 is live. Otherwise both mechanisms claim the same flow
        //     and we fill twice off one aggressor.
        if (r.cls == Class::Trade && r.printable &&
            strictly_through(e.side, e.price, r.print_price) &&
            (!r.known || r.side == e.side) && !opposite_reaches_us(e, post)) {
            trade_class(e, r.shares, r.ts, Trigger::Through, fills, post);
            return;
        }
        // Everything else must be at our exact side and price to matter.
        if (!r.known || r.side != e.side || r.resting_price != e.price) return;

        if (r.cls == Class::Trade) {
            trade_class(e, r.shares, r.ts, Trigger::Execution, fills, post);
            if (cfg_.model == Model::Mbo) note_trade_for_mbo(e, r);
        } else if (r.cls == Class::Cancel) {
            cancel_class(e, r.shares, cancel_is_ahead(e, r));
        }
        // Class::Add joins the back: no effect on anyone already resting.
    }

    // Is the other side already resting at or through our price? If so R1 owns
    // this flow and R3 must stand down.
    static bool opposite_reaches_us(const Entry& e, const book::Book* post) {
        if (post == nullptr) return false;
        int32_t best = 0;
        const bool have = e.side == Side::Buy ? post->best_ask(&best) : post->best_bid(&best);
        return have && at_or_through(e.side, e.price, best);
    }

    // THE line that separates the models.
    bool cancel_is_ahead(const Entry& e, const Resolved& r) const {
        switch (cfg_.model) {
            case Model::Optimistic:  return true;
            case Model::Pessimistic: return false;
            case Model::Mbo:         return in_ahead_set(e.id, r.ref);
            case Model::Naive:       return false;   // unreachable
        }
        return false;
    }

    bool in_ahead_set(uint64_t entry_id, uint64_t ref) const {
        auto it = ahead_sets_.find(entry_id);
        if (it == ahead_sets_.end()) return false;
        return it->second.find(ref) != it->second.end();
    }

    // A trade naming a reference we did not record, while we still believe
    // something is ahead of us, is a priority anomaly relative to the displayed
    // FIFO — display-price sliding, non-displayed ranked interest, or an order
    // routed away and returned. `mbo` still advances (an execution at our price
    // is unambiguous evidence that displayed liquidity there traded); it records
    // the anomaly, and the share-weighted rate is the honest measure of how far
    // `mbo` is from truth. The aggregated models cannot even detect it.
    void note_trade_for_mbo(const Entry& e, const Resolved& r) {
        auto it = ahead_sets_.find(e.id);
        if (it == ahead_sets_.end()) return;
        auto hit = it->second.find(r.ref);
        if (hit == it->second.end()) {
            if (e.ahead > 0) {
                ++priority_anomalies_;
                anomaly_shares_ += r.shares;
            }
            return;
        }
        // Partial removal decrements and keeps the entry. Erasing on any hit
        // drops the rest of that order out of `ahead` and makes mbo fill early.
        if (r.shares >= hit->second) {
            it->second.erase(hit);
        } else {
            hit->second -= r.shares;
        }
    }

    void naive_commit(Entry& e, const Resolved& r, std::vector<SimFill>* fills,
                      const book::Book* post) {
        if (r.cls != Class::Trade || !r.printable) return;
        if (r.type == 'Q') return;   // never on a cross: see below
        if (r.type == 'P' && !cfg_.naive_fills_hidden) return;
        if (!at_or_through(e.side, e.price, r.print_price)) return;

        // min(printed, display), NOT the whole order. "Fill whenever the market
        // trades at your price" taken literally lets a 500-share quote fill in
        // full against a 100-share print, inventing 400 shares that never
        // traded — and then naive is not an upper bound on anything.
        const uint32_t fill = static_cast<uint32_t>(std::min<uint64_t>(r.shares, e.display));
        if (fill == 0) return;
        e.display -= fill;
        if (fills != nullptr) {
            fills->push_back(SimFill{e.id, e.side, e.price, fill, r.ts, Trigger::Naive,
                                     Liquidity::Added, 0});
        }
        if (r.type == 'P') {
            ++naive_hidden_fills_;
            naive_hidden_shares_ += fill;
        }
        if (e.display == 0 && e.hidden > 0) refresh_iceberg(e, post);
    }

    // ---- the clamp ----------------------------------------------------------

    // The true shares ahead of us are a subset of what is actually resting at our
    // price plus our own earlier orders there, so clamping can only remove
    // over-estimates and can never make a model more optimistic than the truth.
    // It also makes "the level emptied" need no code of its own: the limit goes
    // to zero and we are at the front.
    //
    // own_live_ahead is not optional. Our orders are not in book::Book, so
    // shares_at() cannot see them; clamping to the raw level total silently
    // deletes our own earlier orders from `ahead` and double-fills a two-order
    // quoting strategy.
    void clamp_all(const book::Book& post) {
        for (Entry& e : entries_) {
            if (!e.live || e.display == 0) continue;
            const uint64_t limit = post.shares_at(to_char(e.side), e.price) +
                                   own_live_ahead(e.side, e.price, e.arrived_ns);
            if (e.ahead > limit) {
                ++clamp_events_;
                clamp_shares_ += e.ahead - limit;
                e.ahead = limit;
            }
        }
    }

    // Our own other orders at the same price that arrived strictly earlier.
    uint64_t own_live_ahead(Side side, Price4 price, uint64_t arrived_ns) const {
        uint64_t total = 0;
        for (const Entry& e : entries_) {
            if (!e.live || e.display == 0) continue;
            if (e.side != side || e.price != price) continue;
            if (e.arrived_ns < arrived_ns) total += e.display;
        }
        return total;
    }

    void reap() {
        for (Entry& e : entries_) {
            if (e.live && e.display == 0 && e.hidden == 0) e.live = false;
        }
    }

    QueueConfig cfg_;
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, AheadSet> ahead_sets_;

    char state_ = '\0';
    bool tradable_ = false;

    uint64_t clamp_events_ = 0;
    uint64_t clamp_shares_ = 0;
    uint64_t priority_anomalies_ = 0;
    uint64_t anomaly_shares_ = 0;
    uint64_t hidden_inside_shares_ = 0;
    uint64_t naive_hidden_fills_ = 0;
    uint64_t naive_hidden_shares_ = 0;
};

}  // namespace itchbook::sim
