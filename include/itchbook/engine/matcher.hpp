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
        : book_(tick, 20, refs_capacity) {}

    // ---- submission ---------------------------------------------------------

    Result submit(const Request& req) {
        Result r;
        r.first_fill = fills_.size();

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

    // ---- queries ------------------------------------------------------------

    const std::vector<Fill>& fills() const { return fills_; }
    const std::unordered_map<uint64_t, Meta>& orders() const { return orders_; }
    book::Book& book() { return book_; }
    const book::Book& book() const { return book_; }

    const Meta* find(uint64_t id) const {
        auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    bool best_bid(int32_t* out) const { return book_.best_bid(out); }
    bool best_ask(int32_t* out) const { return book_.best_ask(out); }
    bool crossed() const { return book_.crossed(); }

    // ---- invariant accounting ----------------------------------------------
    //
    // Every share that enters the engine is filled, cancelled, or still resting.
    // These three are what the property test balances.

    uint64_t shares_submitted() const { return submitted_; }
    // Counts both sides of every trade, so it balances against shares_submitted.
    // For traded volume in the usual sense, halve it.
    uint64_t shares_filled() const { return filled_total_; }
    uint64_t volume_traded() const { return filled_total_ / 2; }
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
        return book_.best_order(to_char(opposite(req.side))) != nullptr;
    }

    // How much of this order could trade right now. Used only by fill-or-kill,
    // so it walks the book rather than being maintained incrementally.
    uint32_t available(const Request& req, Type type) {
        uint32_t total = 0;
        std::vector<book::LevelView> levels;
        book_.top(to_char(opposite(req.side)), 64, &levels);
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
            const book::Order* front = book_.best_order(other);
            if (front == nullptr) break;
            if (!crosses(taker.req, type, front->price)) break;

            uint64_t maker_id = front->ref;
            auto mit = orders_.find(maker_id);
            if (mit == orders_.end()) break;   // not ours; cannot happen in-engine
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

            book_.take(maker_id, qty);
            maker.resting -= qty;
            maker.filled += qty;
            taker.filled += qty;
            filled_total_ += 2 * qty;   // credited to both sides
            remaining -= qty;

            fills_.push_back(Fill{taker.req.id, maker_id, taker.req.owner,
                                  maker.req.owner, price, qty, maker.sequence});
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
        book_.add(m.req.id, to_char(m.req.side), m.req.price, slice);
        m.in_book = true;
        transition(m.state, State::PartiallyFilled);
    }

    void rest(Meta& m, uint32_t remaining) {
        const uint32_t slice =
            (m.req.display > 0 && m.req.display < remaining) ? m.req.display : remaining;
        m.resting = slice;
        m.hidden = remaining - slice;
        book_.add(m.req.id, to_char(m.req.side), m.req.price, slice);
        m.in_book = true;
    }

    void cancel_meta(Meta& m) {
        const uint32_t gone = m.resting + m.hidden;
        if (m.in_book) {
            book_.remove(m.req.id);
            m.in_book = false;
        }
        m.resting = 0;
        m.hidden = 0;
        m.cancelled += gone;
        cancelled_total_ += gone;
        // A stop that never triggered is still parked; drop it.
        for (size_t i = 0; i < pending_stops_.size(); ++i) {
            if (pending_stops_[i] == m.req.id) {
                pending_stops_[i] = pending_stops_.back();
                pending_stops_.pop_back();
                break;
            }
        }
        transition(m.state, State::Cancelled);
    }

    // A trade may take the price through a resting stop, which then becomes a
    // live order and may itself trade through another. Loop until quiet.
    bool fire_stops() {
        bool any = false;
        bool again = true;
        while (again) {
            again = false;
            for (size_t i = 0; i < pending_stops_.size();) {
                auto it = orders_.find(pending_stops_[i]);
                if (it == orders_.end() || is_terminal(it->second.state)) {
                    pending_stops_[i] = pending_stops_.back();
                    pending_stops_.pop_back();
                    continue;
                }
                Meta& m = it->second;
                if (!stop_is_triggered(m.req)) {
                    ++i;
                    continue;
                }
                pending_stops_[i] = pending_stops_.back();
                pending_stops_.pop_back();

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

    book::Book book_;
    std::unordered_map<uint64_t, Meta> orders_;
    std::vector<Fill> fills_;
    std::vector<uint64_t> pending_stops_;

    uint64_t seq_ = 0;
    uint64_t submitted_ = 0;
    uint64_t filled_total_ = 0;     // counts both sides of every fill
    uint64_t cancelled_total_ = 0;
    int32_t last_trade_ = 0;
    bool has_last_trade_ = false;
};

}  // namespace itchbook::engine
