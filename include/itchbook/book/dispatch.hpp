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

}  // namespace itchbook::book
