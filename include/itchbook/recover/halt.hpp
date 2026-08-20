#pragma once
//
// halt.hpp — the invariant that is only true some of the time.
//
// "bid < ask" is the first thing anyone asserts about an order book, and it is
// wrong. During a halt it is routinely false, legally, and a validator that
// does not know this fires on every halted symbol and teaches whoever is on
// call to ignore it.
//
// The mechanism is simple once stated. In a halt the book keeps accepting
// orders — that is what the quotation-only period is FOR, it is how the
// reopening price is discovered — and nothing executes. So bids accumulate
// above asks with no matching to clear them, and the book crosses. It uncrosses
// at the resume, when the halt cross runs as a single auction and prints as a
// 'Q' cross trade.
//
// So the invariant is conditional, and both halves matter:
//
//   * Crossed while HALTED is expected. Counted, reported, not an error.
//   * Crossed while TRADING is a defect — in the book, the parser, or the
//     feed — and is the thing the unconditional assertion was reaching for.
//
// A single check that ignores the session state either misses the second case
// or drowns in the first. Phase 7's requirement is "never silently wrong", and
// an alert nobody reads is a quieter kind of silence than no alert at all.
//
#include <cstdint>

#include "itchbook/book/book.hpp"

namespace itchbook::recover {

// NASDAQ trading action states, from the 'H' message.
enum class Session : uint8_t {
    Unknown,        // before the first 'H' — not tradable, and not known to be halted
    Trading,        // 'T'
    Halted,         // 'H' — halted across all markets
    QuotationOnly,  // 'Q' — quotes accepted, nothing trades; the reopening period
    Paused,         // 'P' — LULD pause on a NASDAQ-listed name
};

inline Session session_from(char state) {
    switch (state) {
        case 'T': return Session::Trading;
        case 'H': return Session::Halted;
        case 'Q': return Session::QuotationOnly;
        case 'P': return Session::Paused;
        default:  return Session::Unknown;
    }
}

inline const char* to_string(Session s) {
    switch (s) {
        case Session::Unknown:       return "unknown";
        case Session::Trading:       return "trading";
        case Session::Halted:        return "halted";
        case Session::QuotationOnly: return "quotation-only";
        case Session::Paused:        return "paused";
    }
    return "?";
}

// Only 'T' trades. Quotation-only is the state a symbol sits in while its
// reopening auction is being built, and treating it as tradable is how a
// backtest fills orders during a halt.
inline bool tradable(Session s) { return s == Session::Trading; }

struct HaltStats {
    uint64_t halts = 0;                    // entries into a non-trading state
    uint64_t resumes = 0;                  // returns to Trading
    uint64_t halted_ns = 0;                // total feed time not tradable
    uint64_t messages_while_halted = 0;
    uint64_t halt_crosses = 0;             // 'Q' cross trades with type 'H'

    // The conditional invariant, both sides of it.
    uint64_t crossed_while_halted = 0;     // legal
    uint64_t crossed_while_trading = 0;    // a defect
    int32_t deepest_cross_while_halted = 0;
    int32_t deepest_cross_while_trading = 0;

    // Was the book still crossed the moment trading resumed? The auction is
    // supposed to clear it, so a non-zero count here means either the cross
    // print was missed or the book is wrong.
    uint64_t crossed_at_resume = 0;
};

class HaltTracker {
public:
    // An 'H' trading-action message.
    void on_trading_action(char state, uint64_t ts) {
        const Session next = session_from(state);
        if (next == session_) return;
        accrue(ts);
        // Both guarded on having actually seen the previous state. A process
        // that starts up while a symbol is already halted goes Unknown ->
        // Halted, which is not a halt it witnessed, and Unknown -> Trading is
        // not a resume from anything. Counting either inflates the tally for a
        // restarted process specifically — which is the process this phase is
        // about, so getting it wrong here would be self-inflicted.
        if (session_ == Session::Trading && !tradable(next)) ++stats_.halts;
        if (session_ != Session::Unknown && !tradable(session_) && tradable(next)) {
            ++stats_.resumes;
            resuming_ = true;
        }
        session_ = next;
        last_ts_ = ts;
    }

    // A 'Q' cross trade. Type 'H' is the halt cross — the auction that reopens
    // a halted symbol and is the thing that uncrosses the book.
    void on_cross(char type, uint64_t) {
        if (type == 'H') ++stats_.halt_crosses;
    }

    // Time passed and nothing else happened. Split out from observe() because
    // halted time accrues whether or not there is a book to look at — a
    // heartbeat during a halt is still fifteen seconds of halt.
    void observe_at(uint64_t ts) {
        accrue(ts);
        last_ts_ = ts;
    }

    // Called after each message, with the book as it now stands. This is where
    // the conditional invariant is actually evaluated, because whether a
    // crossed book is a problem depends on state this object holds and the
    // book does not.
    void observe(const book::Book& b, uint64_t ts) {
        observe_at(ts);
        if (!tradable(session_)) ++stats_.messages_while_halted;

        int32_t bid = 0;
        int32_t ask = 0;
        const bool two_sided = b.best_bid(&bid) && b.best_ask(&ask);
        const bool crossed = two_sided && bid > ask;
        const int32_t depth = crossed ? bid - ask : 0;

        if (crossed) {
            if (tradable(session_)) {
                ++stats_.crossed_while_trading;
                if (depth > stats_.deepest_cross_while_trading) {
                    stats_.deepest_cross_while_trading = depth;
                }
            } else {
                ++stats_.crossed_while_halted;
                if (depth > stats_.deepest_cross_while_halted) {
                    stats_.deepest_cross_while_halted = depth;
                }
            }
        }
        // Checked on the first observation after a resume, not at the transition
        // itself: the cross print and the state change arrive as separate
        // messages, so asking at the transition asks before the auction has
        // been applied.
        if (resuming_) {
            resuming_ = false;
            if (crossed) ++stats_.crossed_at_resume;
        }
    }

    Session session() const { return session_; }
    bool is_tradable() const { return tradable(session_); }
    const HaltStats& stats() const { return stats_; }

private:
    void accrue(uint64_t ts) {
        if (last_ts_ == 0 || ts < last_ts_) return;
        if (!tradable(session_) && session_ != Session::Unknown) {
            stats_.halted_ns += ts - last_ts_;
        }
    }

    Session session_ = Session::Unknown;
    HaltStats stats_;
    uint64_t last_ts_ = 0;
    bool resuming_ = false;
};

}  // namespace itchbook::recover
