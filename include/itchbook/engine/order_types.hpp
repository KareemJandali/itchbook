#pragma once
//
// order_types.hpp — what an order is, and what may happen to it.
//
// Phase 5 is where the project stops replaying someone else's market and starts
// being one. Everything here describes *our* orders, not the feed's.
//
// The state machine is the part worth reading. An order is in exactly one state
// at all times, and only certain moves are legal. Illegal moves assert rather
// than quietly corrupting the accounting — a fill applied to a cancelled order
// is a bug that must stop the program, not a share count that no longer adds up.
//
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace itchbook::engine {

enum class Side : uint8_t { Buy, Sell };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline char to_char(Side s) { return s == Side::Buy ? 'B' : 'S'; }

enum class Type : uint8_t {
    Limit,       // rests at its price until filled or cancelled
    Market,      // takes whatever is there; never rests
    IOC,         // immediate-or-cancel: take what you can, drop the rest
    FOK,         // fill-or-kill: all of it right now, or none of it
    StopMarket,  // dormant until the market trades through the stop price
    StopLimit,   // ...then becomes a limit order
};

inline bool is_stop(Type t) { return t == Type::StopMarket || t == Type::StopLimit; }

// Only limit orders rest. Everything else either trades immediately or dies.
inline bool can_rest(Type t) { return t == Type::Limit; }

enum class State : uint8_t {
    New,              // created, not yet through the engine
    Accepted,         // resting in the book, nothing filled
    PartiallyFilled,  // some filled, remainder resting or about to be cancelled
    Filled,           // terminal
    Cancelled,        // terminal
    Rejected,         // terminal — never touched the book
};

inline bool is_terminal(State s) {
    return s == State::Filled || s == State::Cancelled || s == State::Rejected;
}

inline const char* to_string(State s) {
    switch (s) {
        case State::New:             return "New";
        case State::Accepted:        return "Accepted";
        case State::PartiallyFilled: return "PartiallyFilled";
        case State::Filled:          return "Filled";
        case State::Cancelled:       return "Cancelled";
        case State::Rejected:        return "Rejected";
    }
    return "?";
}

// The legal moves. Nothing leaves a terminal state, and nothing re-enters New.
inline bool legal_transition(State from, State to) {
    if (is_terminal(from)) return false;
    switch (from) {
        case State::New:
            return to == State::Accepted || to == State::PartiallyFilled ||
                   to == State::Filled || to == State::Cancelled || to == State::Rejected;
        case State::Accepted:
            return to == State::PartiallyFilled || to == State::Filled ||
                   to == State::Cancelled;
        case State::PartiallyFilled:
            return to == State::PartiallyFilled || to == State::Filled ||
                   to == State::Cancelled;
        default:
            return false;
    }
}

// Every state change goes through here. An illegal move is a bug in the engine,
// so it aborts rather than being reported and swallowed.
inline void transition(State& s, State to) {
    assert(legal_transition(s, to) && "illegal order state transition");
    s = to;
}

// Self-trade prevention. When an incoming order would match against resting
// liquidity from the same owner, one of them has to give way.
enum class Stp : uint8_t {
    None,          // allowed to trade with itself
    CancelNewest,  // the incoming order's remainder is cancelled
    CancelOldest,  // the resting order is cancelled, matching continues
    CancelBoth,    // both die
};

enum class Reject : uint8_t {
    None,
    ZeroQuantity,
    InvalidPrice,       // a limit or stop order without a usable price
    DisplayTooLarge,    // iceberg display exceeds total quantity
    FokUnfillable,      // fill-or-kill that could not be filled in full
    NoLiquidity,        // a market order with nothing to trade against
    DuplicateId,
    UnknownOrder,       // cancel for an id the engine has never seen
};

inline const char* to_string(Reject r) {
    switch (r) {
        case Reject::None:            return "None";
        case Reject::ZeroQuantity:    return "ZeroQuantity";
        case Reject::InvalidPrice:    return "InvalidPrice";
        case Reject::DisplayTooLarge: return "DisplayTooLarge";
        case Reject::FokUnfillable:   return "FokUnfillable";
        case Reject::NoLiquidity:     return "NoLiquidity";
        case Reject::DuplicateId:     return "DuplicateId";
        case Reject::UnknownOrder:    return "UnknownOrder";
    }
    return "?";
}

struct Request {
    uint64_t id = 0;
    uint64_t owner = 0;          // for self-trade prevention
    Side side = Side::Buy;
    Type type = Type::Limit;
    int32_t price = 0;           // Price(4); ignored for Market
    int32_t stop_price = 0;      // for stop types
    uint32_t quantity = 0;
    // Iceberg: how much of `quantity` is visible at a time. 0 means fully
    // displayed. When a slice is exhausted the next one joins the *back* of the
    // queue, which is what a real exchange does and what makes icebergs cost
    // queue position rather than being free.
    uint32_t display = 0;
    Stp stp = Stp::None;
};

struct Fill {
    uint64_t taker = 0;
    uint64_t maker = 0;
    uint64_t taker_owner = 0;
    uint64_t maker_owner = 0;
    int32_t price = 0;      // always the resting order's price
    uint32_t shares = 0;
    // The maker's arrival sequence at the moment it traded. Sequences increase
    // monotonically and a refreshed iceberg slice takes a new one, so within a
    // price level these must come out strictly increasing — that is price-time
    // priority, stated in a form a property test can check.
    uint64_t maker_sequence = 0;
    // One number per aggression, shared by every fill it produces. This is the
    // join key the phase-12.6 cross-protocol differential uses to pair an OUCH
    // Executed with the ITCH E describing the same trade -- without it the two
    // streams cannot be matched up at all.
    uint64_t match_number = 0;
    // True when the counterparty is not in this engine: a historical order on
    // the shared book, or the phase-12.1 aggressor. Exactly one side of such a
    // fill is ours, which is why filled_total_ moves by qty rather than 2*qty.
    // Not inferable from taker == 0: validate() has no rule against an order
    // whose id is legitimately zero.
    bool external = false;
};

struct Result {
    State state = State::New;
    Reject reject = Reject::None;
    uint32_t filled = 0;     // shares traded
    uint32_t resting = 0;    // shares left in the book
    uint32_t cancelled = 0;  // shares that will never trade
    size_t first_fill = 0;   // index into Matcher::fills()
    size_t fill_count = 0;
    bool triggered_stops = false;

    bool accepted() const { return reject == Reject::None; }
};

}  // namespace itchbook::engine
