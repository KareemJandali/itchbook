#pragma once
//
// event.hpp — one feed message, resolved into what it means for a queue.
//
// E, C, X, D and U name an order by reference and carry nothing else. Where that
// order was resting, and how big it was, exist only in the book BEFORE the
// message is applied — and for a D, or an E that zeroes the order, they are
// unrecoverable afterwards. So the driver resolves first, applies second, and
// commits to the queue models third.
//
// The one distinction everything else hangs off: TRADE-CLASS events are shares
// that traded, and can spill into a fill for us once they have eaten the queue
// ahead. CANCEL-CLASS events are shares that withdrew, and must never fill us.
// Running both through one "consume the queue, then fill from the remainder"
// path is the single most likely double-count bug in this phase: it fills you
// off a large delete at your price.
//
#include <cstdint>

#include "itchbook/sim/money.hpp"

namespace itchbook::sim {

enum class Side : uint8_t { Buy, Sell };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline char to_char(Side s) { return s == Side::Buy ? 'B' : 'S'; }
inline Side from_char(char c) { return c == 'B' ? Side::Buy : Side::Sell; }

// Is `px` at or through `limit`, from the point of view of a resting order on
// side `s`? For a bid, a lower price is "through". Defined once and used by
// every fill decision, so that a sign error has one place to hide rather than
// a dozen.
inline bool at_or_through(Side s, Price4 limit, Price4 px) {
    return s == Side::Buy ? px <= limit : px >= limit;
}

// Strictly better than our limit, i.e. in front of us in price priority.
inline bool better_than_ours(Side s, Price4 limit, Price4 px) {
    return s == Side::Buy ? px > limit : px < limit;
}

// Strictly through our limit — a price we would have traded at before the
// market ever reached this one.
inline bool strictly_through(Side s, Price4 limit, Price4 px) {
    return s == Side::Buy ? px < limit : px > limit;
}

enum class Class : uint8_t {
    None,     // no queue effect at all (R, S, H, Q, and most A/F)
    Trade,    // shares traded: consume the queue, then spill into a fill
    Cancel,   // shares withdrew: consume the queue, never fill
    Add,      // shares joined the back: no effect on anyone already resting
};

// A feed message, resolved against the pre-mutation book.
struct Resolved {
    char type = 0;
    Class cls = Class::None;

    bool known = false;      // the reference named an order the book held
    uint64_t ref = 0;

    // Where the affected order was RESTING. For a C this is emphatically not
    // the price the trade printed at, and conflating the two is the mechanism
    // behind two of this phase's worst bugs.
    Side side = Side::Buy;
    Price4 resting_price = 0;
    uint32_t shares = 0;     // shares removed by this message

    // Printable trade information, for the naive model and for markouts. A
    // print price differs from the resting price on a repriced C.
    bool printable = false;
    Price4 print_price = 0;

    uint64_t ts = 0;         // nanoseconds since midnight Eastern

    bool trades() const { return cls == Class::Trade; }
    bool cancels() const { return cls == Class::Cancel; }
};

// What caused a simulated fill. Reported per fill, because a run dominated by
// price-priority triggers is a run in which the queue assumption is not what is
// being measured, and the reader has to be able to see that.
enum class Trigger : uint8_t {
    Execution,   // a trade at our price ate through the queue and reached us
    Lock,        // the other side came to our price (R1/R2)
    Through,     // a trade printed strictly through our price (R3)
    Naive,       // the naive model's touch fill
    Taking,      // we crossed the spread deliberately
};

struct SimFill {
    uint64_t order_id = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t shares = 0;
    uint64_t ts = 0;
    Trigger trigger = Trigger::Execution;
    Liquidity liquidity = Liquidity::Added;
    uint64_t ahead_at_arrival = 0;   // what we were behind when we joined
};

}  // namespace itchbook::sim
