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

// Spec message lengths (payload bytes, including the type byte). Used to assert
// the length prefix agrees with the type — a mismatch means desync.
constexpr int spec_length(char t) {
    switch (t) {
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
        default:  return -1;  // metadata type we don't model yet
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

}  // namespace itchbook::itch
