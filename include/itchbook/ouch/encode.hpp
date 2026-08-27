#pragma once
//
// encode.hpp — writing OUCH 4.2, the inverse of messages.hpp.
//
// Every function here is the exact inverse of an accessor in ouch/messages.hpp,
// at the same offsets, for the same nine core message types. See that file's
// banner for the evidence class every offset carries (spec-only, triangulated
// three independent ways, no live session exists to confirm further) and for
// why OUCH gets its own be16/32/64 rather than sharing itch's or emit's.
//
// Unlike ITCH, this project never only READ this protocol — a gateway (12.5)
// has to answer inbound messages, and a strategy (12.7) has to send them, so
// both directions need an encoder from the day this file is written, not
// added later the way emit/itch_encode.hpp was for ITCH.
//
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchbook::ouch::encode {

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}

// Left-justified, right-padded with SPACES — not NUL. Section 1.2: "Alpha
// fields are left-justified and padded on the right with spaces." Token
// fields are alphanumeric and share this padding rule by direct textual
// adjacency in the same Data Types section, though the sentence is not
// re-stated for Token specifically; every Token field below is treated the
// same as an Alpha field for padding purposes on that basis.
inline void put_alpha(uint8_t* p, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s != nullptr && s[i] != '\0'; ++i) p[i] = static_cast<uint8_t>(s[i]);
    for (; i < n; ++i) p[i] = ' ';
}

// Copy wire bytes straight across — for echoing a token or symbol that came
// off another OUCH message and is already correctly padded.
inline void put_alpha_raw(uint8_t* p, const uint8_t* wire, size_t n) {
    if (wire == nullptr) { put_alpha(p, nullptr, n); return; }
    std::memcpy(p, wire, n);
}

// The longest message this encoder produces (Replaced, 80 bytes). Callers
// stack-allocate this and never have to think about it again.
inline constexpr size_t kMaxMessage = 80;

// ---- inbound ------------------------------------------------------------------

inline size_t enter_order(uint8_t* o, const char* token, char side, uint32_t shares,
                          const char* stock, int32_t price, uint32_t tif,
                          const char* firm, char display, char capacity,
                          char iso_eligible, uint32_t min_quantity, char cross_type,
                          char customer_type) {
    o[0] = 'O';
    put_alpha(o + 1, token, 14);
    o[15] = static_cast<uint8_t>(side);
    put32(o + 16, shares);
    put_alpha(o + 20, stock, 8);
    put32(o + 28, static_cast<uint32_t>(price));
    put32(o + 32, tif);
    put_alpha(o + 36, firm, 4);
    o[40] = static_cast<uint8_t>(display);
    o[41] = static_cast<uint8_t>(capacity);
    o[42] = static_cast<uint8_t>(iso_eligible);
    put32(o + 43, min_quantity);
    o[47] = static_cast<uint8_t>(cross_type);
    o[48] = static_cast<uint8_t>(customer_type);
    return 49;
}

inline size_t replace_order(uint8_t* o, const char* existing_token,
                            const char* replacement_token, uint32_t shares,
                            int32_t price, uint32_t tif, char display,
                            char iso_eligible, uint32_t min_quantity) {
    o[0] = 'U';
    put_alpha(o + 1, existing_token, 14);
    put_alpha(o + 15, replacement_token, 14);
    put32(o + 29, shares);
    put32(o + 33, static_cast<uint32_t>(price));
    put32(o + 37, tif);
    o[41] = static_cast<uint8_t>(display);
    o[42] = static_cast<uint8_t>(iso_eligible);
    put32(o + 43, min_quantity);
    return 47;
}

inline size_t cancel_order(uint8_t* o, const char* token, uint32_t shares) {
    o[0] = 'X';
    put_alpha(o + 1, token, 14);
    put32(o + 15, shares);
    return 19;
}

// ---- outbound -------------------------------------------------------------------

inline size_t system_event(uint8_t* o, uint64_t ts, char code) {
    o[0] = 'S';
    put64(o + 1, ts);
    o[9] = static_cast<uint8_t>(code);
    return 10;
}

inline size_t accepted(uint8_t* o, uint64_t ts, const uint8_t* token_wire, char side,
                       uint32_t shares, const uint8_t* stock_wire, int32_t price,
                       uint32_t tif, const uint8_t* firm_wire, char display,
                       uint64_t reference_number, char capacity, char iso_eligible,
                       uint32_t min_quantity, char cross_type, char order_state,
                       char bbo_weight) {
    o[0] = 'A';
    put64(o + 1, ts);
    put_alpha_raw(o + 9, token_wire, 14);
    o[23] = static_cast<uint8_t>(side);
    put32(o + 24, shares);
    put_alpha_raw(o + 28, stock_wire, 8);
    put32(o + 36, static_cast<uint32_t>(price));
    put32(o + 40, tif);
    put_alpha_raw(o + 44, firm_wire, 4);
    o[48] = static_cast<uint8_t>(display);
    put64(o + 49, reference_number);
    o[57] = static_cast<uint8_t>(capacity);
    o[58] = static_cast<uint8_t>(iso_eligible);
    put32(o + 59, min_quantity);
    o[63] = static_cast<uint8_t>(cross_type);
    o[64] = static_cast<uint8_t>(order_state);
    o[65] = static_cast<uint8_t>(bbo_weight);
    return 66;
}

inline size_t replaced(uint8_t* o, uint64_t ts, const uint8_t* replacement_token_wire,
                       char side, uint32_t shares, const uint8_t* stock_wire,
                       int32_t price, uint32_t tif, const uint8_t* firm_wire,
                       char display, uint64_t reference_number, char capacity,
                       char iso_eligible, uint32_t min_quantity, char cross_type,
                       char order_state, const uint8_t* previous_token_wire,
                       char bbo_weight) {
    o[0] = 'U';
    put64(o + 1, ts);
    put_alpha_raw(o + 9, replacement_token_wire, 14);
    o[23] = static_cast<uint8_t>(side);
    put32(o + 24, shares);
    put_alpha_raw(o + 28, stock_wire, 8);
    put32(o + 36, static_cast<uint32_t>(price));
    put32(o + 40, tif);
    put_alpha_raw(o + 44, firm_wire, 4);
    o[48] = static_cast<uint8_t>(display);
    put64(o + 49, reference_number);
    o[57] = static_cast<uint8_t>(capacity);
    o[58] = static_cast<uint8_t>(iso_eligible);
    put32(o + 59, min_quantity);
    o[63] = static_cast<uint8_t>(cross_type);
    o[64] = static_cast<uint8_t>(order_state);
    put_alpha_raw(o + 65, previous_token_wire, 14);
    o[79] = static_cast<uint8_t>(bbo_weight);
    return 80;
}

inline size_t canceled(uint8_t* o, uint64_t ts, const uint8_t* token_wire,
                       uint32_t decrement_shares, char reason) {
    o[0] = 'C';
    put64(o + 1, ts);
    put_alpha_raw(o + 9, token_wire, 14);
    put32(o + 23, decrement_shares);
    o[27] = static_cast<uint8_t>(reason);
    return 28;
}

inline size_t executed(uint8_t* o, uint64_t ts, const uint8_t* token_wire,
                       uint32_t executed_shares, int32_t execution_price,
                       char liquidity_flag, uint64_t match_number) {
    o[0] = 'E';
    put64(o + 1, ts);
    put_alpha_raw(o + 9, token_wire, 14);
    put32(o + 23, executed_shares);
    put32(o + 27, static_cast<uint32_t>(execution_price));
    o[31] = static_cast<uint8_t>(liquidity_flag);
    put64(o + 32, match_number);
    return 40;
}

inline size_t rejected(uint8_t* o, uint64_t ts, const uint8_t* token_wire, char reason) {
    o[0] = 'J';
    put64(o + 1, ts);
    put_alpha_raw(o + 9, token_wire, 14);
    o[23] = static_cast<uint8_t>(reason);
    return 24;
}

}  // namespace itchbook::ouch::encode
