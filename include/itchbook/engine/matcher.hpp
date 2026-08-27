#pragma once
//
// matcher.hpp — price-time priority matching.
//
// The book (book/book.hpp) holds resting liquidity and knows nothing about
// order types or ownership. This engine sits on top of it and adds the parts an
// exchange needs: what kinds of order exist, who owns them, what state each one
// is in, and which of two orders trades first.
//
// The rules that matter, and the reasons they are not arbitrary:
//
//   * **A fill happens at the resting order's price, never the incoming one.**
//     The order that was there first set the terms. This is why a marketable
//     limit at $10.50 buying into an offer at $10.00 pays $10.00, and why price
//     improvement accrues to the taker.
//
//   * **Within a price, oldest trades first.** That is the "time" in price-time
//     priority, and the whole reason phase 6 exists: your position in that queue
//     decides whether you get filled at all.
//
//   * **An iceberg's next slice joins the back of the queue.** Hiding size costs
//     you queue position; if it did not, everyone would hide everything.
//
//   * **Only limit orders rest.** Market, IOC and FOK either trade on arrival or
//     die. Stops are not in the book at all until the market trades through
//     their trigger.
//
// Order metadata lives beside the book rather than inside it: book::Order is 40
// bytes for cache reasons and has no room for an owner, a hidden quantity or a
// state, and the ITCH replay path must not pay for fields only the engine uses.
//
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/engine/order_types.hpp"

namespace itchbook::engine {

class Matcher {
public:
    // Everything the engine knows about one order that the book does not.
    struct Meta {
        Request req;
        State state = State::New;
        uint32_t filled = 0;
        uint32_t cancelled = 0;
        uint32_t resting = 0;     // displayed, i.e. what the book is holding
        uint32_t hidden = 0;      // iceberg remainder, not in the book
        uint64_t sequence = 0;    // arrival order; refreshed slices get a new one
        bool in_book = false;
        // True once the order's quantity has been counted into the engine. A
        // stop parked awaiting its trigger has not entered yet, and a rejected
        // order never does.
        bool entered = false;
    };

    // `refs_capacity` sizes the book's order-reference table. It is exposed
    // because constructing a Book is not free: the table is allocated and
    // zeroed up front, so the default of a million slots costs 16MB per
    // Matcher. That is right for a trading day and badly wrong for a test that
    // builds hundreds of thousands of small books.
    explicit Matcher(int32_t tick = 100, size_t refs_capacity = 1u << 20)
        : owned_(tick, 20, refs_capacity), book_(&owned_) {}

    // Borrowing: match against a book somebody else owns and mutates.
    //
    // docs/phase12-design.md section 4 requires exactly ONE order book per
    // symbol, holding historical and strategy orders in the same price levels
    // and the same queue. Phase 12.1's replayer mutates books belonging to a
    // BookSet; a Matcher that always built its own private Book would have the
    // gateway quoting into, and measuring a price collar against, a book
    // containing only the strategy's own orders -- which is the two-book
    // topology the design doc rules out by name: "Two books ... would put your
    // order in a queue of one and hand it a fill rate nothing in reality
    // supports."
    //
    // The book must outlive the Matcher. Nothing here checks that, for the
    // same reason Book's own borrowing constructors do not check that their
    // Storage outlives them: the owner is a BookSet or a test scope that
    // encloses both, and a lifetime this direct is better stated than
    // instrumented.
    explicit Matcher(book::Book& borrowed, int32_t tick = 100)
        : owned_(tick, 20, 1), book_(&borrowed) {}

    // ---- submission ---------------------------------------------------------

    Result submit(const Request& req) {
        Result r;
        r.first_fill = fills_.size();
        // One match number per aggression, shared by every fill it produces --
        // the rule split.hpp already follows when it replays a historical
        // execution across several makers, and the join key the 12.6
        // cross-protocol differential needs to pair an OUCH Executed with the
        // ITCH E describing the same trade.
        ++match_no_;

        Reject bad = validate(req);
        if (bad != Reject::None) {
            Meta m;
            m.req = req;
            transition(m.state, State::Rejected);
            r.state = State::Rejected;
            r.reject = bad;
            // A rejected order never enters the book, but it is still recorded
            // so that "every order is in exactly one state" holds over the whole
            // population rather than only the ones that worked.
            orders_.emplace(req.id, m);
            return r;
        }

        Meta& m = orders_.emplace(req.id, Meta{}).first->second;
        m.req = req;
        m.sequence = ++seq_;

        // A stop order does not participate until the market trades through its
        // trigger, so it is parked rather than matched.
        if (is_stop(req.type)) {
            if (stop_is_triggered(req)) {
                return run(m, r, effective_type(req.type));
            }
            transition(m.state, State::Accepted);
            m.resting = 0;
            pending_stops_.push_back(req.id);
            r.state = State::Accepted;
            return r;
        }

        return run(m, r, req.type);
    }

    // ---- cancellation -------------------------------------------------------

    bool cancel(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        Meta& m = it->second;
        if (is_terminal(m.state)) return false;
        cancel_meta(m);
        return true;
    }

    // ---- a fill somebody else performed against one of our orders -----------
    //
    // The phase-12.1 aggressor walks the shared book and takes shares from a
    // strategy order resting ahead of the historical order the feed named. It
    // used to call book.take() directly, which left Meta stale, the client
    // uninformed, and -- measured -- conserves_shares() returning true across
    // the whole divergence, because that invariant only checks Meta against
    // itself and never against the book.
    //
    // Returns the shares actually applied, which is the caller's single source
    // of truth: clamping against Meta::resting here rather than against
    // book::Order::shares means a Meta/book desync surfaces as a wrong return
    // value instead of propagating silently.
    //
    // Deliberately does NOT fire stops. A stop elected here would re-enter
    // run()/match() on the shared book in the middle of the replayer's
    // in-flight aggressor walk, and could consume the very historical order
    // the feed named. Stops are armed (last_trade_) and fired later, from
    // pump_stops(), at a quiescent point.
    // `match` is the match number the FEED gave this execution. Passing it in
    // rather than issuing one here is what lets the OUCH Executed this fill
    // produces and the ITCH 'E' the replayer publishes for it carry the same
    // value, which is the only thing that makes the two streams joinable.
    uint32_t apply_external_fill(uint64_t id, uint32_t shares, int32_t price,
                                 uint64_t match) {
        if (shares == 0) return 0;
        auto it = orders_.find(id);
        if (it == orders_.end()) return 0;
        Meta& m = it->second;
        // Each guard is load-bearing. A parked stop is unreachable from the
        // replayer (it is not in the book at all), but a direct mis-call would
        // break conserves_shares()'s !entered branch rather than being caught.
        if (!m.entered || is_terminal(m.state) || !m.in_book || m.resting == 0) return 0;
        // BEFORE take(): a take() on a reference the book does not hold bumps
        // Book::unknown_ref_, and the phase-12.2 gate compares that counter
        // field-by-field against the phase-9 book.
        if (book_->find(id) == nullptr) return 0;

        const uint32_t qty = shares < m.resting ? shares : m.resting;

        book_->take(id, qty);
        m.resting -= qty;
        m.filled += qty;
        filled_total_ += qty;        // ONE side is ours; the aggressor is not
        external_filled_ += qty;
        fills_.push_back(Fill{0, id, 0, m.req.owner, price, qty, m.sequence,
                              match, true});
        last_trade_ = price;
        has_last_trade_ = true;

        if (m.resting == 0) {
            m.in_book = false;
            if (m.hidden > 0) refresh_iceberg(m);
            else transition(m.state, State::Filled);
        } else {
            transition(m.state, State::PartiallyFilled);
        }
        return qty;
    }

    // Stops elected by external fills, fired at a quiescent point. See
    // apply_external_fill's comment for why never inline.
    bool pump_stops() { return fire_stops(); }

    // ---- queries ------------------------------------------------------------

    const std::vector<Fill>& fills() const { return fills_; }
    const std::unordered_map<uint64_t, Meta>& orders() const { return orders_; }
    book::Book& book() { return *book_; }
    const book::Book& book() const { return *book_; }

    const Meta* find(uint64_t id) const {
        auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    bool best_bid(int32_t* out) const { return book_->best_bid(out); }
    bool best_ask(int32_t* out) const { return book_->best_ask(out); }
    bool crossed() const { return book_->crossed(); }

    // ---- invariant accounting ----------------------------------------------
    //
    // Every share that enters the engine is filled, cancelled, or still resting.
    // These three are what the property test balances.

    uint64_t shares_submitted() const { return submitted_; }
    // Counts both sides of every trade, so it balances against shares_submitted.
    // For traded volume in the usual sense, halve it.
    uint64_t shares_filled() const { return filled_total_; }
    // Internal fills credit both sides, external fills only ours, so the
    // halving applies to the internal part alone.
    uint64_t volume_traded() const {
        return (filled_total_ - external_filled_) / 2 + external_filled_;
    }
    uint64_t shares_cancelled() const { return cancelled_total_; }

    uint64_t shares_resting() const {
        uint64_t total = 0;
        for (const auto& [id, m] : orders_) {
            (void)id;
            total += m.resting + m.hidden;
        }
        return total;
    }

    // Every share that entered the engine is filled, cancelled, or still
    // resting — per order, and in aggregate. The per-order half is the stronger
    // check: a global total can balance while two individual orders are wrong
    // in opposite directions.
    // Meta agrees with the BOOK.
    //
    // conserves_shares() checks Meta against itself and is necessary but
    // demonstrably not sufficient: it returned true while a strategy order had
    // been removed from the book by the replayer's aggressor and Meta still
    // claimed 100 shares resting. This is the predicate that sees that class
    // of bug, and it is cheap enough to assert after every fuzz operation.
    //
    // Only the orders this engine owns are checked. On a shared book the
    // historical orders have no Meta by design, so the reverse sweep -- every
    // resting order is known to us -- is false here and belongs to a caller
    // that knows which references are ours.
    bool agrees_with_book() const {
        for (const auto& [id, m] : orders_) {
            const book::Order* o = book_->find(id);
            if (m.in_book) {
                if (o == nullptr || o->shares != m.resting) return false;
            } else if (o != nullptr) {
                return false;
            }
        }
        return true;
    }

    bool conserves_shares() const {
        uint64_t filled = 0;
        uint64_t cancelled = 0;
        uint64_t resting = 0;
        for (const auto& [id, m] : orders_) {
            (void)id;
            if (!m.entered) {
                // Never counted in, so it must not have moved any shares.
                if (m.filled != 0 || m.cancelled != 0 || m.resting != 0 || m.hidden != 0) {
                    return false;
                }
                continue;
            }
            if (m.filled + m.cancelled + m.resting + m.hidden != m.req.quantity) {
                return false;
            }
            filled += m.filled;
            cancelled += m.cancelled;
            resting += m.resting + m.hidden;
        }
        return submitted_ == filled + cancelled + resting &&
               filled == filled_total_ && cancelled == cancelled_total_;
    }

private:
    // ---- validation ---------------------------------------------------------

    Reject validate(const Request& req) const {
        if (orders_.count(req.id) != 0) return Reject::DuplicateId;
        if (req.quantity == 0) return Reject::ZeroQuantity;
        if (req.display > req.quantity) return Reject::DisplayTooLarge;
        if (req.type != Type::Market && req.type != Type::StopMarket && req.price <= 0) {
            return Reject::InvalidPrice;
        }
        if (is_stop(req.type) && req.stop_price <= 0) return Reject::InvalidPrice;
        return Reject::None;
    }

    static Type effective_type(Type t) {
        return t == Type::StopMarket ? Type::Market : Type::Limit;
    }

    // A stop triggers once the market has traded at or through its price: a buy
    // stop when the last trade reaches up to it, a sell stop when it falls to it.
    bool stop_is_triggered(const Request& req) const {
        if (!has_last_trade_) return false;
        return req.side == Side::Buy ? last_trade_ >= req.stop_price
                                     : last_trade_ <= req.stop_price;
    }

    // ---- the matching loop --------------------------------------------------

    Result& run(Meta& m, Result& r, Type type) {
        uint32_t remaining = m.req.quantity;

        // Both rejection cases are settled before a single share is counted
        // into the engine. Deciding afterwards would mean unwinding the
        // accounting, and unwinding is how share counts stop adding up.
        //
        // Which state that refusal lands in depends on where the order came
        // from. Rejected means "never touched the book", so it is only
        // reachable from New. A stop arrives here a second time: it was parked
        // as Accepted when it was submitted, and Accepted -> Rejected is not a
        // move the state machine permits — it aborted the process. An order
        // that has already been accepted and then cannot trade is CANCELLED.
        // Same share count either way; the difference is a word that stays
        // true and a transition that is legal.
        const State refused = (m.state == State::New) ? State::Rejected : State::Cancelled;

        // Fill-or-kill is all-or-nothing: a partial fill that then unwinds is
        // not the same thing, and would leave prints on the tape that never
        // happened.
        if (type == Type::FOK && available(m.req, type) < remaining) {
            transition(m.state, refused);
            r.state = refused;
            r.reject = Reject::FokUnfillable;
            return r;
        }
        // A market order with nothing to trade against never had a chance to
        // rest in the first place.
        if (type == Type::Market && !has_liquidity(m.req)) {
            transition(m.state, refused);
            r.state = refused;
            r.reject = Reject::NoLiquidity;
            return r;
        }

        submitted_ += m.req.quantity;
        m.entered = true;

        bool stp_killed_taker = false;
        remaining = match(m, type, remaining, &stp_killed_taker);

        r.filled = m.filled;
        r.fill_count = fills_.size() - r.first_fill;

        if (remaining > 0 && can_rest(type) && !stp_killed_taker) {
            rest(m, remaining);
            r.resting = m.resting + m.hidden;
            const State want = m.filled > 0 ? State::PartiallyFilled : State::Accepted;
            // A triggered stop-limit that rests is already Accepted; re-asserting
            // the same state is a no-op, not a transition.
            if (m.state != want) transition(m.state, want);
        } else if (remaining > 0) {
            // Market, IOC, FOK and STP-killed orders do not rest.
            m.cancelled += remaining;
            cancelled_total_ += remaining;
            r.cancelled = remaining;
            transition(m.state, State::Cancelled);
        } else {
            transition(m.state, State::Filled);
        }

        r.state = m.state;
        r.triggered_stops = fire_stops();
        return r;
    }

    // Would this order cross the given resting price?
    static bool crosses(const Request& req, Type type, int32_t resting_price) {
        if (type == Type::Market) return true;
        return req.side == Side::Buy ? req.price >= resting_price
                                     : req.price <= resting_price;
    }

    bool has_liquidity(const Request& req) {
        return book_->best_order(to_char(opposite(req.side))) != nullptr;
    }

    // How much of this order could trade right now. Used only by fill-or-kill,
    // so it walks the book rather than being maintained incrementally.
    uint32_t available(const Request& req, Type type) {
        uint32_t total = 0;
        std::vector<book::LevelView> levels;
        book_->top(to_char(opposite(req.side)), 64, &levels);
        for (const auto& lv : levels) {
            if (!crosses(req, type, lv.price)) break;
            total += static_cast<uint32_t>(lv.shares);
            if (total >= req.quantity) return total;
        }
        return total;
    }

    // Consume resting liquidity from the front of the opposite side.
    uint32_t match(Meta& taker, Type type, uint32_t remaining, bool* stp_killed) {
        const char other = to_char(opposite(taker.req.side));

        while (remaining > 0) {
            const book::Order* front = book_->best_order(other);
            if (front == nullptr) break;
            if (!crosses(taker.req, type, front->price)) break;

            uint64_t maker_id = front->ref;
            auto mit = orders_.find(maker_id);

            // A maker this engine does not own.
            //
            // While the Matcher had a private book this could not happen, and
            // the code said so. Phase 12.5 gave it a book shared with the
            // phase-12.1 replayer, per docs/phase12-design.md section 4, and
            // "not ours" became the ORDINARY case: most resting orders are
            // historical and have no Meta. Breaking out here meant a
            // marketable strategy order did not fill at all -- it rested
            // through the opposite side and left the book crossed, measured
            // and confirmed before this was written.
            //
            // So an external maker trades normally. What it does NOT get is
            // Meta bookkeeping (there is none), a state transition, or an
            // iceberg refresh (its hidden quantity, if any, belongs to
            // whoever owns it). The book reduction and the Fill are the whole
            // of it, and the Fill is marked external so a consumer can tell
            // the counterparty is outside this engine.
            if (mit == orders_.end()) {
                const uint32_t qty = remaining < front->shares ? remaining : front->shares;
                const int32_t price = front->price;
                book_->take(maker_id, qty);
                taker.filled += qty;
                filled_total_ += qty;        // ONE side is ours, not two
                external_filled_ += qty;
                remaining -= qty;
                fills_.push_back(Fill{taker.req.id, maker_id, taker.req.owner, 0,
                                      price, qty, 0, match_no_, true});
                last_trade_ = price;
                has_last_trade_ = true;
                continue;
            }
            Meta& maker = mit->second;

            // Self-trade prevention, decided before any shares move.
            if (taker.req.stp != Stp::None && maker.req.owner == taker.req.owner) {
                switch (taker.req.stp) {
                    case Stp::CancelNewest:
                        *stp_killed = true;
                        return remaining;
                    case Stp::CancelOldest:
                        cancel_meta(maker);
                        continue;
                    case Stp::CancelBoth:
                        cancel_meta(maker);
                        *stp_killed = true;
                        return remaining;
                    case Stp::None:
                        break;
                }
            }

            const uint32_t qty = remaining < front->shares ? remaining : front->shares;
            const int32_t price = front->price;   // the resting order sets it

            book_->take(maker_id, qty);
            maker.resting -= qty;
            maker.filled += qty;
            taker.filled += qty;
            filled_total_ += 2 * qty;   // credited to both sides
            remaining -= qty;

            fills_.push_back(Fill{taker.req.id, maker_id, taker.req.owner,
                                  maker.req.owner, price, qty, maker.sequence,
                                  match_no_, false});
            last_trade_ = price;
            has_last_trade_ = true;

            if (maker.resting == 0) {
                maker.in_book = false;
                if (maker.hidden > 0) {
                    refresh_iceberg(maker);
                } else {
                    transition(maker.state, State::Filled);
                }
            } else {
                transition(maker.state, State::PartiallyFilled);
            }
        }
        return remaining;
    }

    // The visible slice is gone; show the next one — at the back of the queue.
    void refresh_iceberg(Meta& m) {
        const uint32_t slice = m.hidden < m.req.display ? m.hidden : m.req.display;
        m.hidden -= slice;
        m.resting = slice;
        m.sequence = ++seq_;   // priority is lost, and the sequence records it
        book_->add(m.req.id, to_char(m.req.side), m.req.price, slice);
        m.in_book = true;
        transition(m.state, State::PartiallyFilled);
    }

    void rest(Meta& m, uint32_t remaining) {
        const uint32_t slice =
            (m.req.display > 0 && m.req.display < remaining) ? m.req.display : remaining;
        m.resting = slice;
        m.hidden = remaining - slice;
        book_->add(m.req.id, to_char(m.req.side), m.req.price, slice);
        m.in_book = true;
    }

    void cancel_meta(Meta& m) {
        const uint32_t gone = m.resting + m.hidden;
        if (m.in_book) {
            book_->remove(m.req.id);
            m.in_book = false;
        }
        m.resting = 0;
        m.hidden = 0;
        m.cancelled += gone;
        cancelled_total_ += gone;
        // A stop that never triggered is still parked; drop it. Erased in
        // place rather than swapped with the back: pending_stops_ is in
        // arrival order and fire_stops() depends on that, so a swap-pop here
        // would reorder the survivors and hand a later stop the priority of
        // the cancelled one. The vector holds parked stops only, so it is
        // short and the shift is cheaper than the bug.
        for (size_t i = 0; i < pending_stops_.size(); ++i) {
            if (pending_stops_[i] == m.req.id) {
                pending_stops_.erase(pending_stops_.begin() + static_cast<long>(i));
                break;
            }
        }
        transition(m.state, State::Cancelled);
    }

    // A trade may take the price through a resting stop, which then becomes a
    // live order and may itself trade through another. Loop until quiet.
    //
    // ORDER MATTERS HERE, and it is the whole reason this loop is not a simple
    // scan-and-swap-pop. One trade can elect several stops at once. They all
    // join the book at that instant, so among themselves they must queue in
    // ARRIVAL order — the order they were submitted and parked — exactly as two
    // limit orders sent at the same price would.
    //
    // pending_stops_ is in arrival order because submit() push_backs. Removing
    // an element by swapping the back into its slot destroys that: park stops
    // 1, 2, 3 and elect all three, and the list goes [1,2,3] -> fire 1 -> [3,2]
    // -> fire 3 -> fire 2, resting them 1, 3, 2. Order 2 arrived before order 3
    // and was elected at the same instant, yet 3 takes priority and fills
    // first. Every removal below therefore erases in place.
    bool fire_stops() {
        bool any = false;
        bool again = true;
        while (again) {
            again = false;
            for (size_t i = 0; i < pending_stops_.size();) {
                auto it = orders_.find(pending_stops_[i]);
                if (it == orders_.end() || is_terminal(it->second.state)) {
                    pending_stops_.erase(pending_stops_.begin() + static_cast<long>(i));
                    continue;
                }
                Meta& m = it->second;
                if (!stop_is_triggered(m.req)) {
                    ++i;
                    continue;
                }
                pending_stops_.erase(pending_stops_.begin() + static_cast<long>(i));

                Result sub;
                sub.first_fill = fills_.size();
                // A parked stop is not in any queue: it holds no shares and
                // nobody can trade with it. It joins the market at the moment
                // it TRIGGERS, so that is when its arrival sequence is taken —
                // the same rule refresh_iceberg() applies for the same reason.
                // Keeping the sequence it got at submit time would let a stop
                // parked at 09:31 fire at 15:00 and rest ahead of every order
                // that had been queued at that price all day.
                m.sequence = ++seq_;
                // It runs from Accepted; run() moves it on from there. Forcing
                // it back to New would be an illegal transition.
                run(m, sub, effective_type(m.req.type));
                any = true;
                again = true;
                break;
            }
        }
        return any;
    }

    // owned_ is the book in the owning case and an inert 1-slot placeholder in
    // the borrowing one -- a Book has no default constructor and making it
    // optional would put a branch on every access to the hottest structure in
    // the engine. One wasted RefMap slot is the cheaper trade.
    book::Book owned_;
    book::Book* book_;
    std::unordered_map<uint64_t, Meta> orders_;
    std::vector<Fill> fills_;
    std::vector<uint64_t> pending_stops_;

    uint64_t seq_ = 0;
    uint64_t match_no_ = 0;
    uint64_t submitted_ = 0;
    uint64_t filled_total_ = 0;     // both sides of an internal fill, one side
                                    // of an external one
    uint64_t external_filled_ = 0;  // the one-sided part of the above
    uint64_t cancelled_total_ = 0;
    int32_t last_trade_ = 0;
    bool has_last_trade_ = false;
};

}  // namespace itchbook::engine
