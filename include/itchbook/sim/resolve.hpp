#pragma once
//
// resolve.hpp — an ITCH message, plus the pre-mutation book, becomes a queue event.
//
// This is where the phase's single most dangerous rule lives:
//
//   A 'C' fills only when printable == 'Y' AND the print price equals the
//   order's RESTING price. Every other 'C' is cancel-class.
//
// Both halves matter. A non-printable C is already reported elsewhere as part of
// an aggregate cross print, so filling on it double-counts — and on the
// validated MSFT day, 2,500,408 of 6,154,278 shares (40.6%) executed at exactly
// two prices in the two crosses, with participants' displayed orders removed by
// precisely this message shape. A repriced C matched under a regime this model
// does not describe, so treating it as a trade at our level manufactures a fill.
//
// Relevance is decided by the RESTING price, never by the wire price. A C
// printing at 100.02 against an order resting at 100.00 is an event at our level
// if we are at 100.00, and is not one if we are at 100.02.
//
#include <cstdint>

#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/sim/event.hpp"

namespace itchbook::sim {

// `pre` must come from book::apply_ex, i.e. captured BEFORE the mutation.
inline Resolved resolve(char type, const uint8_t* p, const book::PreState& pre) {
    namespace m = itchbook::itch;
    Resolved r;
    r.type = type;
    r.ts = m::timestamp(p);
    r.known = pre.known;
    r.ref = pre.ref;

    switch (type) {
        case 'A':
            r.cls = Class::Add;
            r.side = from_char(m::add_order::side(p));
            r.resting_price = m::add_order::price(p);
            r.shares = m::add_order::shares(p);
            break;

        case 'F':
            r.cls = Class::Add;
            r.side = from_char(m::add_order_mpid::side(p));
            r.resting_price = m::add_order_mpid::price(p);
            r.shares = m::add_order_mpid::shares(p);
            break;

        case 'E':
            // Trades at the resting order's own price, so the print price and
            // the resting price are the same thing here.
            r.cls = Class::Trade;
            r.side = from_char(pre.side);
            r.resting_price = pre.price;
            r.shares = m::order_executed::executed_shares(p);
            r.printable = true;
            r.print_price = pre.price;
            break;

        case 'C': {
            const bool printable = m::order_executed_price::printable(p);
            const Price4 print_px = m::order_executed_price::price(p);
            r.side = from_char(pre.side);
            r.resting_price = pre.price;
            r.shares = m::order_executed_price::executed_shares(p);
            r.printable = printable;
            r.print_price = print_px;
            // THE rule.
            r.cls = (printable && print_px == pre.price) ? Class::Trade : Class::Cancel;
            break;
        }

        case 'X':
            r.cls = Class::Cancel;
            r.side = from_char(pre.side);
            r.resting_price = pre.price;
            r.shares = m::order_cancel::canceled_shares(p);
            break;

        case 'D':
            // A delete removes whatever was left, which only the pre-mutation
            // book knows.
            r.cls = Class::Cancel;
            r.side = from_char(pre.side);
            r.resting_price = pre.price;
            r.shares = pre.shares;
            break;

        case 'U':
            // Delete-then-add. The delete half inherits all of a D's ambiguity;
            // the add half joins the back with certainty and so has no effect on
            // anyone already resting. Only the delete half is modelled here.
            //
            // U is 4.49% of the validated day and D is 44.95%, so between them
            // they are ~49.4% of the feed and the entire source of the
            // optimistic/pessimistic spread. X — the message everyone calls "the
            // ambiguous cancel" — is 0.07%.
            r.cls = Class::Cancel;
            r.side = from_char(pre.side);
            r.resting_price = pre.price;
            r.shares = pre.shares;
            break;

        case 'P':
            // Hidden liquidity. It never sat in the displayed FIFO, so it cannot
            // advance any queue — `known` stays false, which is what keeps it out
            // of the same-price advance path. It is still a printable trade, so
            // the naive model fills on it and a print strictly through our limit
            // still triggers price priority.
            //
            // Its Buy/Sell indicator is deliberately not used: it is the side of
            // the non-displayed order and is widely observed to be constant, and
            // both of this repo's generators hardcode it.
            r.cls = Class::Trade;
            r.known = false;
            r.shares = m::trade::shares(p);
            r.printable = true;
            r.print_price = m::trade::price(p);
            r.resting_price = r.print_price;
            break;

        case 'Q':
            // A cross never touches the book. Millions of shares at one price:
            // a model that fills on it measures the auction and nothing else.
            r.cls = Class::None;
            r.shares = static_cast<uint32_t>(m::cross_trade::shares(p));
            r.printable = true;
            r.print_price = m::cross_trade::price(p);
            r.resting_price = r.print_price;
            break;

        default:
            r.cls = Class::None;
            break;
    }
    return r;
}

}  // namespace itchbook::sim
