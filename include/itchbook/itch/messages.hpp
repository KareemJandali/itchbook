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

// The ITCH 5.0 types this book does NOT model, and why each is safe to skip.
// Listing them is the point: an unmodelled type that nobody wrote down is
// indistinguishable from one nobody thought about, and two of these bear
// directly on tradability, which is the property recover/halt.hpp exists to
// get right.
//
//   'B'  Broken Trade          busts a previously printed trade. No book
//                              effect — trades never entered the book — but it
//                              does mean a day's printed volume can be revised
//                              after the fact.
//   'h'  Operational Halt      a venue-level halt of a symbol, separate from
//                              the 'H' trading action. TRADABILITY-RELEVANT:
//                              halt.hpp derives its session state from 'H'
//                              alone, so an operational halt is currently not
//                              reflected. It is rare and did not occur in the
//                              day this project validates against, which makes
//                              it a known gap rather than a measured one.
//   'V'  MWCB Decline Level    market-wide circuit-breaker trigger levels,
//   'W'  MWCB Status           and the announcement that one was breached. A
//                              breach halts the whole market, so this is also
//                              tradability-relevant and also not modelled.
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
// the length prefix agrees with the type — a mismatch means desync. Unmodelled
// types are listed here too: the handler ignores them, but the frame is still
// checked, so a desync that lands inside a metadata message is caught at that
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
        // Framed and length-checked, but not modelled. See the note above.
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

// Whether this book interprets the message, as opposed to merely framing it.
// Used by tools that want to report what a feed contained but was ignored.
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
