#pragma once
//
// itch_encode.hpp — writing ITCH 5.0, which until phase 12 this project only read.
//
// Every function here is the exact inverse of a decoder in itch/messages.hpp,
// and the pairing is the point: a field written at the wrong offset or in the
// wrong endianness is caught by decoding it again, which is what
// tests/test_itch_encode.cpp does for every type. The offsets are therefore
// NOT repeated as literals -- they are taken from the decoders' own namespaces
// where a constant exists, and where one does not the encoder and the decoder
// sit in the same test.
//
// Message lengths are the one thing that cannot be derived from the decoders,
// because a decoder only ever reads up to its last field. They come from
// python/make_sample.py, which carries them as comments beside each builder,
// and they agree with the last-field offsets here. The same caveat that file
// states applies with equal force: building a message from the same length
// constant that parses it says nothing about whether the constant matches
// NASDAQ's wire. What is evidence is that a 268-million-message real file
// frames cleanly against these lengths -- see the census -- and what is not is
// the three types no real file in hand contains ('h', 'W', 'B').
//
// Timestamps are REPLAY time, taken from the message being described, never
// wall clock. docs/phase12-design.md section 6 is why: a day replayed at 50x
// still has to publish a stream whose internal timestamps describe the original
// session, because that is what a consumer's book arithmetic depends on.
//
#include <cstdint>
#include <cstring>

namespace itchbook::emit {

// ---- primitives --------------------------------------------------------------

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

inline void put48(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 6; ++i) p[i] = static_cast<uint8_t>(v >> (40 - 8 * i));
}

inline void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}

// 8 chars, left justified, SPACE padded -- not NUL padded. The distinction is
// load-bearing: BookSet::set_directory copies a fixed 8 and trims trailing
// spaces, so a NUL-padded symbol arrives with embedded NULs and compares
// unequal to the same symbol read off a real file.
inline void put_stock(uint8_t* p, const char* sym, size_t n = 8) {
    size_t i = 0;
    for (; i < n && sym != nullptr && sym[i] != '\0'; ++i) p[i] = static_cast<uint8_t>(sym[i]);
    for (; i < n; ++i) p[i] = ' ';
}

// Copy 8 wire bytes straight across, for the case where the symbol came off
// another ITCH message and is already space-padded.
inline void put_stock_raw(uint8_t* p, const uint8_t* wire, size_t n = 8) {
    if (wire == nullptr) {
        put_stock(p, nullptr, n);
        return;
    }
    std::memcpy(p, wire, n);
}

// type(1) locate(2) tracking(2) timestamp(6) -- common to every message.
inline void header(uint8_t* p, char type, uint16_t locate, uint16_t tracking, uint64_t ts_ns) {
    p[0] = static_cast<uint8_t>(type);
    put16(p + 1, locate);
    put16(p + 3, tracking);
    put48(p + 5, ts_ns);
}

// ---- the messages ------------------------------------------------------------
//
// Each returns the encoded length, so a caller can frame it without a second
// table of sizes to keep in step.

inline size_t system_event(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts, char code) {
    header(o, 'S', loc, trk, ts);
    o[11] = static_cast<uint8_t>(code);
    return 12;
}

inline size_t stock_directory(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                              const uint8_t* stock_wire, char category, char financial,
                              uint32_t round_lot) {
    std::memset(o, 0, 39);
    header(o, 'R', loc, trk, ts);
    put_stock_raw(o + 11, stock_wire);
    o[19] = static_cast<uint8_t>(category);
    o[20] = static_cast<uint8_t>(financial);
    put32(o + 21, round_lot);
    return 39;
}

inline size_t add_order(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                        uint64_t ref, char side, uint32_t shares,
                        const uint8_t* stock_wire, int32_t price) {
    header(o, 'A', loc, trk, ts);
    put64(o + 11, ref);
    o[19] = static_cast<uint8_t>(side);
    put32(o + 20, shares);
    put_stock_raw(o + 24, stock_wire);
    put32(o + 32, static_cast<uint32_t>(price));
    return 36;
}

inline size_t add_order_mpid(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                             uint64_t ref, char side, uint32_t shares,
                             const uint8_t* stock_wire, int32_t price,
                             const uint8_t* mpid_wire) {
    header(o, 'F', loc, trk, ts);
    put64(o + 11, ref);
    o[19] = static_cast<uint8_t>(side);
    put32(o + 20, shares);
    put_stock_raw(o + 24, stock_wire);
    put32(o + 32, static_cast<uint32_t>(price));
    if (mpid_wire != nullptr) std::memcpy(o + 36, mpid_wire, 4);
    else std::memset(o + 36, ' ', 4);
    return 40;
}

inline size_t order_executed(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                             uint64_t ref, uint32_t shares, uint64_t match) {
    header(o, 'E', loc, trk, ts);
    put64(o + 11, ref);
    put32(o + 19, shares);
    put64(o + 23, match);
    return 31;
}

inline size_t order_executed_price(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                                   uint64_t ref, uint32_t shares, uint64_t match,
                                   bool printable, int32_t price) {
    header(o, 'C', loc, trk, ts);
    put64(o + 11, ref);
    put32(o + 19, shares);
    put64(o + 23, match);
    o[31] = static_cast<uint8_t>(printable ? 'Y' : 'N');
    put32(o + 32, static_cast<uint32_t>(price));
    return 36;
}

inline size_t order_cancel(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                           uint64_t ref, uint32_t shares) {
    header(o, 'X', loc, trk, ts);
    put64(o + 11, ref);
    put32(o + 19, shares);
    return 23;
}

inline size_t order_delete(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts, uint64_t ref) {
    header(o, 'D', loc, trk, ts);
    put64(o + 11, ref);
    return 19;
}

inline size_t order_replace(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                            uint64_t orig_ref, uint64_t new_ref, uint32_t shares, int32_t price) {
    header(o, 'U', loc, trk, ts);
    put64(o + 11, orig_ref);
    put64(o + 19, new_ref);
    put32(o + 27, shares);
    put32(o + 31, static_cast<uint32_t>(price));
    return 35;
}

inline size_t trade(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                    uint64_t ref, char side, uint32_t shares,
                    const uint8_t* stock_wire, int32_t price, uint64_t match) {
    header(o, 'P', loc, trk, ts);
    put64(o + 11, ref);
    o[19] = static_cast<uint8_t>(side);
    put32(o + 20, shares);
    put_stock_raw(o + 24, stock_wire);
    put32(o + 32, static_cast<uint32_t>(price));
    put64(o + 36, match);
    return 44;
}

inline size_t cross_trade(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                          uint64_t shares, const uint8_t* stock_wire, int32_t price,
                          uint64_t match, char cross_type) {
    header(o, 'Q', loc, trk, ts);
    put64(o + 11, shares);
    put_stock_raw(o + 19, stock_wire);
    put32(o + 27, static_cast<uint32_t>(price));
    put64(o + 31, match);
    o[39] = static_cast<uint8_t>(cross_type);
    return 40;
}

inline size_t trading_action(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                             const uint8_t* stock_wire, char state, const uint8_t* reason_wire) {
    header(o, 'H', loc, trk, ts);
    put_stock_raw(o + 11, stock_wire);
    o[19] = static_cast<uint8_t>(state);
    o[20] = ' ';   // reserved
    if (reason_wire != nullptr) std::memcpy(o + 21, reason_wire, 4);
    else std::memset(o + 21, ' ', 4);
    return 25;
}

inline size_t operational_halt(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts,
                               const uint8_t* stock_wire, char market_code, char action) {
    header(o, 'h', loc, trk, ts);
    put_stock_raw(o + 11, stock_wire);
    o[19] = static_cast<uint8_t>(market_code);
    o[20] = static_cast<uint8_t>(action);
    return 21;
}

inline size_t mwcb_status(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts, char level) {
    header(o, 'W', loc, trk, ts);
    o[11] = static_cast<uint8_t>(level);
    return 12;
}

inline size_t broken_trade(uint8_t* o, uint16_t loc, uint16_t trk, uint64_t ts, uint64_t match) {
    header(o, 'B', loc, trk, ts);
    put64(o + 11, match);
    return 19;
}

// The longest message this encoder produces. Callers stack-allocate this and
// never have to think about it again.
inline constexpr size_t kMaxMessage = 64;

}  // namespace itchbook::emit
