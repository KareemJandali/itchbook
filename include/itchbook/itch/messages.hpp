#pragma once
//
// messages.hpp — ITCH 5.0 field access.
//
// Rule from the build plan: read fields at explicit byte offsets. Do NOT
// reinterpret_cast the payload to a packed struct — that is alignment and
// strict-aliasing UB, and after inlining it is not faster anyway.
//
// Everything on the wire is big-endian. We are on little-endian x86, so every
// multi-byte integer must be byteswapped on read.
//
#include <cstddef>
#include <cstdint>

namespace itchbook::itch {

// ---- big-endian field readers ------------------------------------------------

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

// Timestamps are 6 bytes: nanoseconds since midnight. No 6-byte integer type
// exists, so assemble by hand.
inline uint64_t be48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

// ---- message types -----------------------------------------------------------

enum class Type : char {
    SystemEvent          = 'S',
    StockDirectory       = 'R',
    AddOrder             = 'A',
    AddOrderMPID         = 'F',
    OrderExecuted        = 'E',
    OrderExecutedPrice   = 'C',
    OrderCancel          = 'X',
    OrderDelete          = 'D',
    OrderReplace         = 'U',
    TradeNonCross        = 'P',
    CrossTrade           = 'Q',
    StockTradingAction   = 'H',
};

// The ITCH 5.0 types this book does not model — the eleven modelled() answers
// false for — and for each either what is done with it instead or why dropping
// it is safe. Listing them is the point: an unmodelled type that nobody wrote
// down is indistinguishable from one nobody thought about, and two of them
// decide whether a symbol may trade at all, which is the property
// BookSet::tradable() exists to get right.
//
// Three of them are acted on all the same, and only by the C++ side. None of
// the three is a book mutation, so python/reference/book.py has nothing to
// mirror and modelled() stays false for them — which is exactly what keeps the
// two implementations comparable — and dispatch.hpp routes them to the BookSet
// instead, where per-symbol and session state that is not a book lives.
//
//   'h'  Operational Halt      a venue-level halt of a symbol, separate from
//                              the 'H' trading action. ACTED ON, per symbol:
//                              set_operational_halt() records it and
//                              BookSet::tradable() returns false while it
//                              stands. Entries into it are counted as well, so
//                              a run reports how many fired even for symbols
//                              that resumed before the close. (recover/halt.hpp
//                              does still read 'H' alone. That is not this gap:
//                              its Session classifies crossed books, which is a
//                              narrower question than tradability.)
//   'W'  MWCB Status           the announcement that a market-wide circuit
//                              breaker level was breached. ACTED ON, market
//                              wide: set_mwcb_breached() records the level and
//                              tradable() returns false for every symbol while
//                              it stands. KNOWN LIMITATION — the breach is
//                              treated as PERMANENT, and it is not. Level 1 and
//                              2 halt the market for a fixed period and then it
//                              reopens, and nothing in the feed says so, so
//                              after a breach this calls every symbol
//                              untradable for the rest of the session including
//                              a stretch when the market is trading again.
//                              Wrong in the safe direction for a predicate
//                              whose consumers are risk checks, but a
//                              limitation rather than a conservatism to be
//                              proud of. The argument and what fixing it would
//                              take are written out beside tradable() in
//                              book_set.hpp.
//   'B'  Broken Trade          busts a previously printed trade. No book
//                              effect — trades never entered the book — and
//                              no correction is applied either: undoing a print
//                              means revising volume, VWAP and possibly the
//                              close, which needs the match number of every
//                              trade the day printed. COUNTED per symbol
//                              instead, so a daily bar that disagrees with a
//                              vendor's can be explained rather than argued
//                              with. See note_broken_trade() in book_set.hpp.
//
// The rest are framed, length-checked and dropped.
//
//   'V'  MWCB Decline Level    the circuit-breaker trigger levels themselves,
//                              published near the start of the session. Only a
//                              breach changes tradability, and the 'W' that
//                              announces one carries the level it breached.
//   'Y'  Reg SHO Restriction   short-sale price-test state. Constrains who may
//                              post, not what the book contains.
//   'L'  Market Participant    per-MPID quoting state. TotalView carries the
//        Position              orders themselves as 'A'/'F'.
//   'K'  IPO Quoting Period    first-day quoting windows.
//   'J'  LULD Auction Collar   limit-up/limit-down collar prices.
//   'N'  RPII                  retail price improvement interest, indicative.
//   'I'  NOII                  auction imbalance, indicative. The auction
//                              itself arrives as a 'Q' cross trade, which IS
//                              modelled.
//   'O'  Direct Listing with   price discovery for a direct listing with a
//        Capital Raise         capital raise. Indicative, like 'I': the
//                              resulting auction prints as a 'Q'.
//
// Spec message lengths (payload bytes, including the type byte). Used to assert
// the length prefix agrees with the type — a mismatch means desync. Every type
// above is listed here too, acted on or dropped: the frame is checked either
// way, so a desync that lands inside a metadata message is caught at that
// message rather than at the next one this book happens to care about.
//
// VERIFIED AGAINST REAL BYTES on 12302019.NASDAQ_ITCH50.gz — the whole day,
// every symbol: 268,744,780 messages, 8.25 GB, no length mismatch. That run
// exercised 18 of these types. The six unmodelled ones it reached are marked
// below; 'V' appeared exactly once, which was enough to prove it is not
// transposed with 'W' (they are both circuit-breaker messages and differ by 23
// bytes, so a swap throws on the first occurrence).
//
// The five marked UNCONFIRMED are read from the spec and have never been
// checked against a real message, because that day contained none: nothing
// breached a circuit-breaker level, operationally halted, busted a trade, or
// published retail price improvement, and 'O' postdates the file. No generator
// here emits them either, so CI cannot reach them. Run itch_census on a more
// eventful day before trusting them.
constexpr int spec_length(char t) {
    switch (t) {
        // Modelled.
        case 'S': return 12;
        case 'R': return 39;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'H': return 25;
        // Framed and length-checked, and not modelled. Three of them — 'h',
        // 'W' and 'B' — are still acted on at the BookSet level; the rest are
        // dropped. See the note above.
        case 'Y': return 20;  // seen: 9,013
        case 'L': return 26;  // seen: 215,161
        case 'V': return 35;  // seen: 1 — proves V/W not transposed
        case 'W': return 12;  // UNCONFIRMED
        case 'K': return 28;  // seen: 3
        case 'J': return 35;  // seen: 34
        case 'h': return 21;  // UNCONFIRMED
        case 'B': return 19;  // UNCONFIRMED
        case 'I': return 50;  // seen: 4,024,315
        case 'N': return 20;  // UNCONFIRMED
        case 'O': return 48;  // UNCONFIRMED
        default:  return -1;  // genuinely unknown — do not guess a length
    }
}

// The name the spec gives a type. Lives here rather than in the one tool that
// first needed it: two tools now print type tables, and a switch copied into
// both is a table that drifts the first time a type is added to one of them.
//
// Empty string for a type not in ITCH 5.0 -- not "Unknown", which reads like a
// name and would sit in a column beside real ones.
constexpr const char* type_name(char t) {
    switch (t) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'A': return "Add Order";
        case 'F': return "Add Order w/ MPID";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed w/ Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'h': return "Operational Halt";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'N': return "Retail Price Improvement";
        case 'O': return "Direct Listing w/ Capital Raise";
        default:  return "";
    }
}

// The twelve types the reference implementation decodes. Used by tools that
// want to report how much of a feed no book acted on.
//
// Decoded is not the same as applied, and the gap is three types wide. Only
// nine of the twelve are a book or volume mutation, which is what apply()
// returns true for and what a run reports as messages_applied; 'R', 'S' and 'H'
// are decoded and inert. So read-minus-applied is the unmodelled count PLUS
// those three, which is the arithmetic scripts/full-day-check.py gates on.
//
// Nor is it the same question as "is it acted on at all". 'h', 'W' and 'B'
// answer false here and are still acted on: none of them is a book mutation for
// python/reference/book.py to mirror, so keeping them out is what keeps this
// predicate an honest statement of what the two implementations share, and
// dispatch.hpp routes them to the BookSet instead. See the note at the top of
// this file for what each of the three does.
constexpr bool modelled(char t) {
    switch (t) {
        case 'S': case 'R': case 'A': case 'F': case 'E': case 'C':
        case 'X': case 'D': case 'U': case 'P': case 'Q': case 'H':
            return true;
        default:
            return false;
    }
}

// Common header: type(1) locate(2) tracking(2) timestamp(6). Timestamp lives at
// offset 5 in every message that carries one.
inline uint64_t timestamp(const uint8_t* p) { return be48(p + 5); }
inline uint16_t stock_locate(const uint8_t* p) { return be16(p + 1); }

// ---- typed accessors for the seven book-mutating messages --------------------

namespace add_order {  // 'A'
inline uint64_t ref(const uint8_t* p)    { return be64(p + 11); }
inline char     side(const uint8_t* p)   { return static_cast<char>(p[19]); }  // 'B' / 'S'
inline uint32_t shares(const uint8_t* p) { return be32(p + 20); }
inline const uint8_t* stock(const uint8_t* p) { return p + 24; }  // 8 chars, space-padded
inline int32_t  price(const uint8_t* p)  { return static_cast<int32_t>(be32(p + 32)); }
}  // namespace add_order

namespace order_executed {  // 'E'
inline uint64_t ref(const uint8_t* p)           { return be64(p + 11); }
inline uint32_t executed_shares(const uint8_t* p) { return be32(p + 19); }
inline uint64_t match_number(const uint8_t* p)  { return be64(p + 23); }
}  // namespace order_executed

namespace order_cancel {  // 'X'
inline uint64_t ref(const uint8_t* p)             { return be64(p + 11); }
inline uint32_t canceled_shares(const uint8_t* p) { return be32(p + 19); }
}  // namespace order_cancel

namespace order_delete {  // 'D'
inline uint64_t ref(const uint8_t* p) { return be64(p + 11); }
}  // namespace order_delete

namespace system_event {  // 'S'
inline char code(const uint8_t* p) { return static_cast<char>(p[11]); }  // 'O','S','Q','M','E','C'
}  // namespace system_event

namespace add_order_mpid {  // 'F' — identical to 'A', plus attribution
inline uint64_t ref(const uint8_t* p)    { return be64(p + 11); }
inline char     side(const uint8_t* p)   { return static_cast<char>(p[19]); }
inline uint32_t shares(const uint8_t* p) { return be32(p + 20); }
inline const uint8_t* stock(const uint8_t* p) { return p + 24; }
inline int32_t  price(const uint8_t* p)  { return static_cast<int32_t>(be32(p + 32)); }
inline const uint8_t* attribution(const uint8_t* p) { return p + 36; }  // 4 chars
}  // namespace add_order_mpid

namespace order_executed_price {  // 'C'
inline uint64_t ref(const uint8_t* p)             { return be64(p + 11); }
inline uint32_t executed_shares(const uint8_t* p) { return be32(p + 19); }
inline uint64_t match_number(const uint8_t* p)    { return be64(p + 23); }
// A non-printable execution still removes shares from the book, but must not
// count toward volume, VWAP or OHLC.
inline bool     printable(const uint8_t* p)       { return p[31] == 'Y'; }
inline int32_t  price(const uint8_t* p)           { return static_cast<int32_t>(be32(p + 32)); }
}  // namespace order_executed_price

namespace order_replace {  // 'U'
// Side and stock are not on the wire — they are inherited from the original
// order. The replacement goes to the back of its level: a replace loses queue
// priority, which is the whole reason phase 6 exists.
inline uint64_t original_ref(const uint8_t* p) { return be64(p + 11); }
inline uint64_t new_ref(const uint8_t* p)      { return be64(p + 19); }
inline uint32_t shares(const uint8_t* p)       { return be32(p + 27); }
inline int32_t  price(const uint8_t* p)        { return static_cast<int32_t>(be32(p + 31)); }
}  // namespace order_replace

namespace trade {  // 'P' — non-cross trade against hidden liquidity. No book effect.
inline uint64_t ref(const uint8_t* p)    { return be64(p + 11); }
inline char     side(const uint8_t* p)   { return static_cast<char>(p[19]); }
inline uint32_t shares(const uint8_t* p) { return be32(p + 20); }
inline const uint8_t* stock(const uint8_t* p) { return p + 24; }
inline int32_t  price(const uint8_t* p)  { return static_cast<int32_t>(be32(p + 32)); }
inline uint64_t match_number(const uint8_t* p) { return be64(p + 36); }
}  // namespace trade

namespace cross_trade {  // 'Q' — opening / closing cross. No book effect.
// Note the 8-byte share count: a cross is far bigger than any single order.
inline uint64_t shares(const uint8_t* p) { return be64(p + 11); }
inline const uint8_t* stock(const uint8_t* p) { return p + 19; }
inline int32_t  price(const uint8_t* p)  { return static_cast<int32_t>(be32(p + 27)); }
inline uint64_t match_number(const uint8_t* p) { return be64(p + 31); }
inline char     cross_type(const uint8_t* p) { return static_cast<char>(p[39]); }  // 'O','C','H','I'
}  // namespace cross_trade

namespace trading_action {  // 'H'
inline const uint8_t* stock(const uint8_t* p) { return p + 11; }
inline char state(const uint8_t* p) { return static_cast<char>(p[19]); }  // 'H','P','Q','T'
inline const uint8_t* reason(const uint8_t* p) { return p + 21; }         // 4 chars
}  // namespace trading_action

namespace stock_directory {  // 'R'
inline const uint8_t* stock(const uint8_t* p) { return p + 11; }
inline char     market_category(const uint8_t* p)  { return static_cast<char>(p[19]); }
inline char     financial_status(const uint8_t* p) { return static_cast<char>(p[20]); }
inline uint32_t round_lot_size(const uint8_t* p)   { return be32(p + 21); }
}  // namespace stock_directory

// ---- the three that bear on whether a symbol traded --------------------------
//
// None of these mutates a book, which is why they were skipped for eight
// phases. Two of them decide whether a symbol may trade at all, and the third
// decides whether a day's printed volume is final — and a single symbol on a
// quiet day reaches none of them, so they went unexamined until a whole file
// was read.
//
// **UNCONFIRMED, all three.** 12302019 contains no 'h', no 'W' and no 'B', so
// these offsets have never been checked against a real message. They are read
// from the spec, exactly like the length constants above them, and they are
// marked here for the same reason: a number that has met real bytes and a
// number that has not are different kinds of claim, and a reader cannot tell
// them apart unless the file says so. The lengths at least fail loudly on
// contact — a wrong offset inside a correct length does not.

namespace operational_halt {  // 'h' — venue-level halt, separate from 'H'
inline const uint8_t* stock(const uint8_t* p) { return p + 11; }
inline char market_code(const uint8_t* p) { return static_cast<char>(p[19]); }  // 'Q','B','X'
inline char action(const uint8_t* p) { return static_cast<char>(p[20]); }       // 'H','T'
}  // namespace operational_halt

namespace mwcb_status {  // 'W' — a market-wide circuit-breaker level was breached
inline char breached_level(const uint8_t* p) { return static_cast<char>(p[11]); }  // '1','2','3'
}  // namespace mwcb_status

namespace broken_trade {  // 'B' — a previously printed trade is busted
inline uint64_t match_number(const uint8_t* p) { return be64(p + 11); }
}  // namespace broken_trade

}  // namespace itchbook::itch
