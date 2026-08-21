#pragma once
//
// dispatch.hpp — maps ITCH messages onto Book mutations.
//
// The book itself takes mutations, not messages, and the parser knows nothing
// about books. This header is the seam between them, and it is deliberately the
// only place that knows both. It mirrors `apply()` in python/reference/book.py
// case for case: when the C++ and the oracle disagree, the two files should be
// readable side by side.
//
#include <cstdint>

#include "itchbook/book/book.hpp"
#include "itchbook/book/book_set.hpp"
#include "itchbook/itch/messages.hpp"

namespace itchbook::book {

// True for the message types the reference implementation decodes. Anything
// else is skipped outright — including for snapshot timing, so that both
// implementations advance their clocks on exactly the same messages.
inline bool modelled(char type) {
    switch (type) {
        case 'S': case 'R': case 'A': case 'F': case 'E': case 'C':
        case 'X': case 'D': case 'U': case 'P': case 'Q': case 'H':
            return true;
        default:
            return false;
    }
}

// Apply one message. Returns true if it counted as a book or volume mutation
// ('R', 'S' and 'H' carry state but mutate nothing).
inline bool apply(Book& b, char type, const uint8_t* p) {
    namespace m = itchbook::itch;
    switch (type) {
        case 'A':
            b.add(m::add_order::ref(p), m::add_order::side(p),
                  m::add_order::price(p), m::add_order::shares(p));
            return true;
        case 'F':
            b.add(m::add_order_mpid::ref(p), m::add_order_mpid::side(p),
                  m::add_order_mpid::price(p), m::add_order_mpid::shares(p));
            return true;
        case 'E':
            b.execute(m::order_executed::ref(p), m::order_executed::executed_shares(p));
            return true;
        case 'C':
            b.execute_with_price(m::order_executed_price::ref(p),
                                 m::order_executed_price::executed_shares(p),
                                 m::order_executed_price::price(p),
                                 m::order_executed_price::printable(p));
            return true;
        case 'X':
            b.cancel(m::order_cancel::ref(p), m::order_cancel::canceled_shares(p));
            return true;
        case 'D':
            b.remove(m::order_delete::ref(p));
            return true;
        case 'U':
            b.replace(m::order_replace::original_ref(p), m::order_replace::new_ref(p),
                      m::order_replace::price(p), m::order_replace::shares(p));
            return true;
        case 'P':
            b.trade(m::trade::price(p), m::trade::shares(p));
            return true;
        case 'Q':
            b.cross(m::cross_trade::price(p), m::cross_trade::shares(p),
                    m::cross_trade::cross_type(p));
            return true;
        case 'H':
            b.set_trading_state(m::trading_action::state(p));
            return false;
        case 'S':
            b.set_system_event(m::system_event::code(p));
            return false;
        default:
            return false;   // 'R' and anything else: metadata, no book effect
    }
}

// ---- the whole feed ----------------------------------------------------------
//
// The same seam, one symbol wider. It routes on the locate the message already
// carries and then hands the message to the single-book apply() above, so there
// is exactly one place in the project that knows what an 'E' does to a book. A
// second copy of that switch would be a second thing to keep in step with
// python/reference/book.py, and the case-for-case correspondence between those
// two files is what makes a disagreement readable.
//
// Two types never reach a book:
//
//   'S'  is the market's session, not a symbol's. It carries stock locate 0, so
//        routing it would file the whole session under whichever symbol owns
//        that code and leave every other summary blank.
//
//   'R'  is the directory. Fields are pulled out here, because this is the file
//        that is allowed to know a wire layout, and handed to the BookSet as
//        values — book_set.hpp never sees a message.
//
// Unmodelled types stop here rather than at the book, and that matters more
// than it looks: BookSet::at() creates a book on first use, so routing an 'I'
// would conjure a book for a symbol that only ever published an auction
// imbalance, and count a message against a directory entry that may not exist
// yet. 1.58% of a real day is unmodelled, so this is thousands of phantom books
// on a live file, not a corner case.
inline bool apply(BookSet& set, char type, const uint8_t* p) {
    namespace m = itchbook::itch;
    switch (type) {
        case 'S':
            set.set_system_event(m::system_event::code(p));
            return false;
        case 'R':
            set.set_directory(m::stock_locate(p),
                              reinterpret_cast<const char*>(m::stock_directory::stock(p)), 8,
                              m::stock_directory::market_category(p),
                              m::stock_directory::financial_status(p),
                              m::stock_directory::round_lot_size(p));
            return false;
        // Three that never touch a book and are not routed to one. Handling
        // them here rather than in modelled() keeps the C++/Python contract
        // exactly where it was: the oracle mirrors apply(Book&), and none of
        // these produces a book mutation for it to mirror.
        case 'h':
            set.set_operational_halt(m::stock_locate(p), m::operational_halt::action(p));
            return false;
        case 'W':
            set.set_mwcb_breached(m::mwcb_status::breached_level(p));
            return false;
        case 'B':
            set.note_broken_trade(m::stock_locate(p));
            return false;
        default:
            if (!modelled(type)) return false;
            return apply(set.at(m::stock_locate(p)), type, p);
    }
}

// ---- pre-mutation state ------------------------------------------------------
//
// E, C, X, D and U carry only an order reference. The order's side, resting
// price and remaining shares live in the book, and for a D — or an E that takes
// the order to zero — they are gone the instant the mutation lands. Anything
// that needs to know *where* a message landed has to look before it applies.
//
// This is not an optimisation, it is a correctness requirement: a simulator
// tracking a queue at one price level cannot otherwise tell whether a cancel it
// just saw was even at its price.
struct PreState {
    bool known = false;      // the reference named an order the book was holding
    char side = 0;           // 'B' or 'S'
    int32_t price = 0;       // the order's RESTING price, which for a C is not
                             // the same as the price the trade printed at
    uint32_t shares = 0;     // displayed shares before this message
    uint64_t ref = 0;
};

// True for the message types that name an existing order by reference.
inline bool references_resting_order(char type) {
    switch (type) {
        case 'E': case 'C': case 'X': case 'D': case 'U': return true;
        default: return false;
    }
}

inline uint64_t referenced_ref(char type, const uint8_t* p) {
    namespace m = itchbook::itch;
    switch (type) {
        case 'E': return m::order_executed::ref(p);
        case 'C': return m::order_executed_price::ref(p);
        case 'X': return m::order_cancel::ref(p);
        case 'D': return m::order_delete::ref(p);
        case 'U': return m::order_replace::original_ref(p);
        default:  return 0;
    }
}

// apply(), with the pre-mutation snapshot the simulator needs.
//
// No BookSet form, deliberately. The simulator trades one symbol: it needs the
// resting price and side of the order a message names, and asking that question
// of 8,700 books at once has no meaning until a strategy exists that quotes
// more than one. Adding the overload now would be a second copy of the probe
// with no caller to keep it honest.
//
// This probes the reference map once here and once inside the mutation, rather
// than being fused into a single probe. That is a deliberate choice backed by
// measurement: phase 4 removed exactly this kind of second, cache-warm probe
// from the delete path and it was worth -0.5%, which is nothing (bench/README.md).
// The second probe walks the same slot chain the first just pulled into L1.
// Fusing them would mean widening Book's API to pass slot indices around, for a
// gain already measured to be absent.
//
// What it does buy is that resolve and apply cannot disagree about which order a
// reference names — a real hazard, since RefMap::insert resolves a duplicate
// reference by last-writer-wins.
inline bool apply_ex(Book& b, char type, const uint8_t* p, PreState* out) {
    *out = PreState{};
    if (references_resting_order(type)) {
        const uint64_t ref = referenced_ref(type, p);
        if (const Order* o = b.find(ref)) {
            out->known = true;
            out->side = static_cast<char>(o->side);
            out->price = o->price;
            out->shares = o->shares;
            out->ref = ref;
        } else {
            out->ref = ref;   // recorded so unknown references can be counted
        }
    }
    return apply(b, type, p);
}

}  // namespace itchbook::book
