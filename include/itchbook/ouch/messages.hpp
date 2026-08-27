#pragma once
//
// messages.hpp — OUCH 4.2 field access.
//
// OUCH is the order-entry protocol: a client sends Enter/Replace/Cancel to an
// exchange, the exchange replies with Accepted/Replaced/Canceled/Executed/
// Rejected. ITCH (itch/messages.hpp) is one-way, market-data-out; OUCH is a
// session with two directions, and the same type byte can mean different
// things in each: 'U' is Replace Order inbound (47 bytes) and Replaced
// outbound (80 bytes). Every accessor below is named for its message, not
// bare on the type byte, so that ambiguity cannot leak into a call site.
//
// SOURCE: "O*U*C*H Version 4.2", NASDAQ, footer-dated October 2025,
// https://www.nasdaqtrader.com/content/technicalsupport/specifications/tradingproducts/ouch4.2.pdf
//
// EVIDENCE CLASS — read this before trusting a number below.
//
// itch/messages.hpp marks a field CONFIRMED when a real trading day's bytes
// have been read at that offset, and UNCONFIRMED when it is spec-only. That
// distinction cannot be drawn here at all: this project has no OUCH session,
// live or recorded, and never will — OUCH requires a live NASDAQ market
// participant connection, which is out of scope for a portfolio project by
// construction. EVERY offset below is therefore spec-only in the sense
// itch/messages.hpp uses that word, for every message, with no exception.
//
// What is NOT spec-only is how carefully the spec was read. The offset and
// length of every field in the nine message types below were extracted from
// the vendor PDF THREE independent ways — a blind reconstruction from
// pre-flattened text, a from-scratch font-aware extractor that found and
// fixed a real bug (the document draws five field-name labels, though never
// an offset or a length, with an embedded Identity-H composite font that a
// naive byte-level reader renders as garbage), and a third pass with
// `pdftotext -layout` — and all three land on identical offsets and lengths
// for all nine messages, with the field tables tiling their message's total
// length exactly, zero gaps, zero overlaps. That triangulation is real
// evidence and is the strongest available without a live session; it is not
// the same claim itch/messages.hpp makes about a byte a real day contained,
// and the header will keep saying so rather than borrowing that word.
//
// One value genuinely did not survive any of the three passes: the Enter
// Order Message's Cross Type field (offset 47, length 1) has a confirmed
// offset and length, but its permitted value characters are only referenced
// ("see Data Types") and the referenced table never appears in the extracted
// text. It is exposed here as an opaque char, unenumerated, for that reason.
//
// CORE SUBSET. Per the build plan: inbound Enter Order (O), Replace Order
// (U), Cancel Order (X); outbound System Event (S), Accepted (A), Replaced
// (U), Canceled (C), Executed (E), Rejected (J). The spec documents six more
// message types this file does not implement — Modify Order (inbound 'M',
// itself marked "greyed out" / not currently offered by the document's own
// footer note), AIQ Cancelled ('D'), Broken Trade ('B'), Executed with
// Reference Price ('G'), Cancel Pending ('P'), Cancel Reject ('I'), Order
// Priority Update ('T') and Order Modified (outbound 'M'). Out of scope for
// this phase, not forgotten.
//
#include <cstddef>
#include <cstdint>

namespace itchbook::ouch {

// ---- big-endian field readers ------------------------------------------------
//
// Not shared with itch::be16/be32/be64: emit/itch_encode.hpp made the same
// choice for its own put16/32/64 rather than reaching into itch/messages.hpp,
// and the reason carries over here — each wire-format module is a
// self-contained statement of one protocol's layout, not a client of another
// protocol's helpers that happen to compute the same thing.
inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// ---- data types (spec section 1.2) --------------------------------------------
//
// "All integer fields are unsigned big-endian (network byte order) binary
// encoded numbers. Alpha fields are left-justified and padded on the right
// with spaces. Token fields are alphanumeric... Tokens are case sensitive."
//
// Price is a fixed-point integer: 6 whole-number digits + 4 decimal digits,
// i.e. decimal price = raw / 10000 — the same scale ITCH's own Price(4) uses,
// which is why it is stored as int32_t here exactly as book::Order::price and
// engine::Request::price already are, and flows into them with no cast.
//
// Two evidence numbers the spec states in decimal AND hex, which is a gift:
// they are a self-checking pair, verified independently below rather than
// copied.
//   max price      $199,999.9900 = 1,999,999,900 = 0x7735939C
//   market sentinel $214,748.3647 = 2,147,483,647 = 0x7FFFFFFF (= INT32_MAX)
// "Orders entered with a price of $200,000.00 or the max integer value will
// also be treated as market orders" — so the sentinel test is price >=
// kMarketOrderThreshold, not price == kMarketOrderPrice alone.
inline constexpr int32_t kMaxPrice = 1999999900;
inline constexpr int32_t kMarketOrderPrice = 2147483647;
inline constexpr int32_t kMarketOrderThreshold = 2000000000;   // $200,000.00
static_assert(kMaxPrice == 0x7735939C, "must match the spec's own stated hex");
static_assert(kMarketOrderPrice == 0x7FFFFFFF, "must match the spec's own stated hex");
static_assert(kMarketOrderPrice > kMaxPrice, "the sentinel must sit above every real price");

// ---- the reference partition: Order Token vs Order Reference Number ----------
//
// This protocol's version of the locate trap, per docs/phase12-design.md, and
// it is worth stating plainly rather than leaving it implicit in two
// similarly-named fields:
//
//   Order Token   — 14 bytes, alphanumeric, CLIENT-chosen, day-unique per
//                   OUCH account. Names an order on EVERY inbound message and
//                   is echoed back on every outbound one. Never appears on
//                   the ITCH wire; it has no meaning outside this session.
//   Reference Number — 8-byte integer, EXCHANGE-assigned, present only in
//                   Accepted and Replaced among the nine core messages. This
//                   is the value that becomes the ITCH order reference the
//                   matcher publishes for this order once 12.2's emitter
//                   describes it — the bridge between OUCH's session-local
//                   string identity and ITCH's wire-level integer identity.
//
// Two disjoint namespaces, two disjoint types (14-byte alnum vs 8-byte
// integer), and nothing here confuses them: no accessor below reads a Token
// field as a number or a Reference Number field as bytes.

// ---- inbound: Enter Order Message ('O', 49 bytes) -----------------------------
namespace enter_order {
inline uint64_t timestamp_absent() { return 0; }   // inbound carries no timestamp
inline const uint8_t* order_token(const uint8_t* p)  { return p + 1; }    //  1..14
inline char            side(const uint8_t* p)         { return static_cast<char>(p[15]); }
inline uint32_t         shares(const uint8_t* p)       { return be32(p + 16); }
inline const uint8_t*  stock(const uint8_t* p)        { return p + 20; }  // 20..27
inline int32_t          price(const uint8_t* p)        { return static_cast<int32_t>(be32(p + 28)); }
inline uint32_t         time_in_force(const uint8_t* p) { return be32(p + 32); }
inline const uint8_t*  firm(const uint8_t* p)         { return p + 36; }  // 36..39
inline char             display(const uint8_t* p)      { return static_cast<char>(p[40]); }
inline char             capacity(const uint8_t* p)     { return static_cast<char>(p[41]); }
inline char             iso_eligible(const uint8_t* p) { return static_cast<char>(p[42]); }
inline uint32_t         min_quantity(const uint8_t* p)  { return be32(p + 43); }
// Offset/length confirmed; permitted values UNCONFIRMED — see file banner.
inline char             cross_type(const uint8_t* p)    { return static_cast<char>(p[47]); }
inline char             customer_type(const uint8_t* p) { return static_cast<char>(p[48]); }
}  // namespace enter_order
inline constexpr size_t kEnterOrderLen = 49;

// ---- inbound: Replace Order Message ('U', 47 bytes) ---------------------------
namespace replace_order {
inline const uint8_t* existing_order_token(const uint8_t* p)     { return p + 1; }   //  1..14
inline const uint8_t* replacement_order_token(const uint8_t* p)  { return p + 15; }  // 15..28
inline uint32_t         shares(const uint8_t* p)        { return be32(p + 29); }
inline int32_t          price(const uint8_t* p)         { return static_cast<int32_t>(be32(p + 33)); }
inline uint32_t         time_in_force(const uint8_t* p)  { return be32(p + 37); }
inline char             display(const uint8_t* p)       { return static_cast<char>(p[41]); }
inline char             iso_eligible(const uint8_t* p)  { return static_cast<char>(p[42]); }
inline uint32_t         min_quantity(const uint8_t* p)   { return be32(p + 43); }
}  // namespace replace_order
inline constexpr size_t kReplaceOrderLen = 47;

// ---- inbound: Cancel Order Message ('X', 19 bytes) -----------------------------
namespace cancel_order {
inline const uint8_t* order_token(const uint8_t* p) { return p + 1; }   // 1..14
// New intended order size; 0 cancels every remaining share.
inline uint32_t         shares(const uint8_t* p)      { return be32(p + 15); }
}  // namespace cancel_order
inline constexpr size_t kCancelOrderLen = 19;

// ---- outbound: System Event Message ('S', 10 bytes) ----------------------------
namespace system_event {
inline uint64_t timestamp(const uint8_t* p) { return be64(p + 1); }
inline char      code(const uint8_t* p)      { return static_cast<char>(p[9]); }
}  // namespace system_event
inline constexpr size_t kSystemEventLen = 10;

// ---- outbound: Accepted Message ('A', 66 bytes) ---------------------------------
namespace accepted {
inline uint64_t         timestamp(const uint8_t* p)     { return be64(p + 1); }
inline const uint8_t*  order_token(const uint8_t* p)   { return p + 9; }    //  9..22
inline char             side(const uint8_t* p)          { return static_cast<char>(p[23]); }
inline uint32_t          shares(const uint8_t* p)        { return be32(p + 24); }
inline const uint8_t*  stock(const uint8_t* p)         { return p + 28; }   // 28..35
inline int32_t           price(const uint8_t* p)         { return static_cast<int32_t>(be32(p + 36)); }
inline uint32_t          time_in_force(const uint8_t* p)  { return be32(p + 40); }
inline const uint8_t*  firm(const uint8_t* p)          { return p + 44; }   // 44..47
inline char              display(const uint8_t* p)       { return static_cast<char>(p[48]); }
// The exchange-assigned identity that later becomes the ITCH order reference.
inline uint64_t          reference_number(const uint8_t* p) { return be64(p + 49); }
inline char              capacity(const uint8_t* p)      { return static_cast<char>(p[57]); }
inline char              iso_eligible(const uint8_t* p)  { return static_cast<char>(p[58]); }
inline uint32_t          min_quantity(const uint8_t* p)   { return be32(p + 59); }
inline char              cross_type(const uint8_t* p)     { return static_cast<char>(p[63]); }
inline char              order_state(const uint8_t* p)    { return static_cast<char>(p[64]); }   // 'L'/'D'
inline char              bbo_weight(const uint8_t* p)     { return static_cast<char>(p[65]); }
}  // namespace accepted
inline constexpr size_t kAcceptedLen = 66;

// ---- outbound: Replaced Message ('U', 80 bytes) ---------------------------------
//
// The single field this triangulation corrected against a first guess: byte 9
// is "Replacement Order Token" in the vendor's own Name column, not the plain
// "Order Token" a positional analogy to Accepted would suggest. It is the
// same 14-byte alphanumeric field either way; the correction is the name a
// reader would reach for, not the offset, and it is recorded here so nobody
// re-derives it wrong from the same analogy.
namespace replaced {
inline uint64_t         timestamp(const uint8_t* p)             { return be64(p + 1); }
inline const uint8_t*  replacement_order_token(const uint8_t* p) { return p + 9; }    //  9..22
inline char              side(const uint8_t* p)                  { return static_cast<char>(p[23]); }
inline uint32_t          shares(const uint8_t* p)                 { return be32(p + 24); }
inline const uint8_t*  stock(const uint8_t* p)                  { return p + 28; }    // 28..35
inline int32_t           price(const uint8_t* p)                  { return static_cast<int32_t>(be32(p + 36)); }
inline uint32_t          time_in_force(const uint8_t* p)           { return be32(p + 40); }
inline const uint8_t*  firm(const uint8_t* p)                   { return p + 44; }    // 44..47
inline char              display(const uint8_t* p)                { return static_cast<char>(p[48]); }
inline uint64_t          reference_number(const uint8_t* p)        { return be64(p + 49); }
inline char              capacity(const uint8_t* p)               { return static_cast<char>(p[57]); }
inline char              iso_eligible(const uint8_t* p)           { return static_cast<char>(p[58]); }
inline uint32_t          min_quantity(const uint8_t* p)            { return be32(p + 59); }
inline char              cross_type(const uint8_t* p)              { return static_cast<char>(p[63]); }
inline char              order_state(const uint8_t* p)             { return static_cast<char>(p[64]); }
inline const uint8_t*  previous_order_token(const uint8_t* p)   { return p + 65; }   // 65..78
inline char              bbo_weight(const uint8_t* p)              { return static_cast<char>(p[79]); }
}  // namespace replaced
inline constexpr size_t kReplacedLen = 80;

// ---- outbound: Canceled Message ('C', 28 bytes) ----------------------------------
namespace canceled {
inline uint64_t         timestamp(const uint8_t* p)  { return be64(p + 1); }
inline const uint8_t*  order_token(const uint8_t* p) { return p + 9; }   // 9..22
// Incremental, not cumulative: shares just removed by this one message.
inline uint32_t          decrement_shares(const uint8_t* p) { return be32(p + 23); }
// See 3.5.1 Cancel Order Reasons: U, I, T, S, D, Q, Z, C, K, H, X, E, F, G.
inline char              reason(const uint8_t* p) { return static_cast<char>(p[27]); }
}  // namespace canceled
inline constexpr size_t kCanceledLen = 28;

// ---- outbound: Executed Message ('E', 40 bytes) -----------------------------------
namespace executed {
inline uint64_t         timestamp(const uint8_t* p)      { return be64(p + 1); }
inline const uint8_t*  order_token(const uint8_t* p)     { return p + 9; }   // 9..22
inline uint32_t          executed_shares(const uint8_t* p) { return be32(p + 23); }
inline int32_t            execution_price(const uint8_t* p) { return static_cast<int32_t>(be32(p + 27)); }
// See 3.7.1 Liquidity Flag Values (many codes; several greyed out over the
// spec's revision history — pass-through char, not enumerated here).
inline char              liquidity_flag(const uint8_t* p) { return static_cast<char>(p[31]); }
inline uint64_t          match_number(const uint8_t* p)    { return be64(p + 32); }
}  // namespace executed
inline constexpr size_t kExecutedLen = 40;

// ---- outbound: Rejected Message ('J', 24 bytes) -------------------------------------
namespace rejected {
inline uint64_t         timestamp(const uint8_t* p)  { return be64(p + 1); }
inline const uint8_t*  order_token(const uint8_t* p) { return p + 9; }   // 9..22
// See 3.10.1 Rejected Order Reasons: a,b,c,C,d,D,e,H,L,m,M,n,N,o,O,q,r,S,T,
// u,v,V,w,W,x,X,y,Z.
inline char              reason(const uint8_t* p) { return static_cast<char>(p[23]); }
}  // namespace rejected
inline constexpr size_t kRejectedLen = 24;

// ---- structural self-check: every message tiles its own length, exactly -----
//
// Independent of the spec, independent of all three extraction passes: given
// only the offsets and lengths declared above, do they cover 0..total_length
// with no gap and no overlap? This is the same check the verification
// workflow ran by hand on the extracted text: it is now permanent, runs at
// compile time, and fails the build the moment a future edit desyncs an
// offset from its neighbour, rather than waiting for a round-trip test to
// notice.
namespace detail {
struct Span { size_t offset; size_t length; };

constexpr bool tiles(const Span* spans, size_t n, size_t total) {
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        if (spans[i].offset != at) return false;
        at += spans[i].length;
    }
    return at == total;
}

inline constexpr Span kEnterOrder[] = {
    {0,1},{1,14},{15,1},{16,4},{20,8},{28,4},{32,4},{36,4},{40,1},
    {41,1},{42,1},{43,4},{47,1},{48,1},
};
inline constexpr Span kReplaceOrder[] = {
    {0,1},{1,14},{15,14},{29,4},{33,4},{37,4},{41,1},{42,1},{43,4},
};
inline constexpr Span kCancelOrder[] = { {0,1},{1,14},{15,4} };
inline constexpr Span kSystemEvent[] = { {0,1},{1,8},{9,1} };
inline constexpr Span kAccepted[] = {
    {0,1},{1,8},{9,14},{23,1},{24,4},{28,8},{36,4},{40,4},{44,4},{48,1},
    {49,8},{57,1},{58,1},{59,4},{63,1},{64,1},{65,1},
};
inline constexpr Span kReplaced[] = {
    {0,1},{1,8},{9,14},{23,1},{24,4},{28,8},{36,4},{40,4},{44,4},{48,1},
    {49,8},{57,1},{58,1},{59,4},{63,1},{64,1},{65,14},{79,1},
};
inline constexpr Span kCanceled[] = { {0,1},{1,8},{9,14},{23,4},{27,1} };
inline constexpr Span kExecuted[] = { {0,1},{1,8},{9,14},{23,4},{27,4},{31,1},{32,8} };
inline constexpr Span kRejected[] = { {0,1},{1,8},{9,14},{23,1} };
}  // namespace detail

static_assert(detail::tiles(detail::kEnterOrder, 14, kEnterOrderLen), "Enter Order: gap or overlap");
static_assert(detail::tiles(detail::kReplaceOrder, 9, kReplaceOrderLen), "Replace Order: gap or overlap");
static_assert(detail::tiles(detail::kCancelOrder, 3, kCancelOrderLen), "Cancel Order: gap or overlap");
static_assert(detail::tiles(detail::kSystemEvent, 3, kSystemEventLen), "System Event: gap or overlap");
static_assert(detail::tiles(detail::kAccepted, 17, kAcceptedLen), "Accepted: gap or overlap");
static_assert(detail::tiles(detail::kReplaced, 18, kReplacedLen), "Replaced: gap or overlap");
static_assert(detail::tiles(detail::kCanceled, 5, kCanceledLen), "Canceled: gap or overlap");
static_assert(detail::tiles(detail::kExecuted, 7, kExecutedLen), "Executed: gap or overlap");
static_assert(detail::tiles(detail::kRejected, 4, kRejectedLen), "Rejected: gap or overlap");

}  // namespace itchbook::ouch
