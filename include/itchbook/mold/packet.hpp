#pragma once
//
// packet.hpp — MoldUDP64 framing.
//
// ITCH says what happened in the market. MoldUDP64 says whether you received
// all of it. They are separate problems and the daily sample files hide the
// second one: NASDAQ ships them already de-MoldUDP'd, as a bare stream of
// length-prefixed messages, so every message is present, in order, exactly
// once, forever. A book built only against those files has never met a dropped
// packet, and "it works on the sample file" is not evidence that it would.
//
// The wire format (MoldUDP64 V1.00), 20-byte header then message blocks:
//
//   session          10 bytes, alphanumeric, space-padded
//   sequence number   8 bytes, big-endian — of the FIRST message in the packet
//   message count     2 bytes, big-endian
//   then `count` blocks of: length (2 bytes, big-endian), then that many bytes
//
// Two counts are special, and both matter for a receiver that has to be
// correct rather than merely usual:
//
//   * **0 is a heartbeat.** It carries no messages, and its sequence number is
//     the next one the sender expects to send. That makes it the only way to
//     learn that nothing has been lost during a quiet period — without
//     heartbeats, "no packets" and "every packet dropped" look identical.
//   * **0xFFFF is end-of-session.** Not a packet with 65,535 messages in it. A
//     receiver that treats the count as a plain integer walks off the end of
//     the buffer here, which is the kind of bug that only fires at 16:00.
//
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "itchbook/itch/messages.hpp"

namespace itchbook::mold {

inline constexpr size_t kHeaderLen = 20;
inline constexpr size_t kSessionLen = 10;
inline constexpr uint16_t kEndOfSession = 0xFFFF;

struct Header {
    char session[kSessionLen + 1] = {};   // NUL-terminated for printing
    uint64_t sequence = 0;
    uint16_t count = 0;

    bool heartbeat() const { return count == 0; }
    bool end_of_session() const { return count == kEndOfSession; }
    // Messages actually carried. End-of-session carries none despite its count.
    uint16_t message_count() const { return end_of_session() ? 0 : count; }
};

inline bool parse_header(const uint8_t* p, size_t len, Header* out) {
    if (p == nullptr || len < kHeaderLen) return false;
    std::memcpy(out->session, p, kSessionLen);
    out->session[kSessionLen] = '\0';
    out->sequence = itch::be64(p + kSessionLen);
    out->count = itch::be16(p + kSessionLen + 8);
    return true;
}

// Walks the message blocks of one packet.
//
// Every step is bounds-checked against the packet's actual length rather than
// trusting the header's count. A truncated packet is a thing that happens — a
// datagram clipped by a middlebox, a capture file cut short — and the count
// field is written by whoever sent it, which on a public feed is not a reason
// to believe it.
class BlockIterator {
public:
    BlockIterator(const uint8_t* packet, size_t len, uint16_t count)
        : p_(packet), len_(len), remaining_(count), off_(kHeaderLen) {}

    // Returns false at the end, or on a truncated block. `truncated()`
    // distinguishes the two, because they mean different things: one is a
    // normal end of packet and the other is corruption.
    bool next(const uint8_t** msg, uint16_t* msg_len) {
        if (remaining_ == 0) return false;
        if (off_ + 2 > len_) {
            truncated_ = true;
            return false;
        }
        const uint16_t n = itch::be16(p_ + off_);
        if (off_ + 2 + n > len_) {
            truncated_ = true;
            return false;
        }
        *msg = p_ + off_ + 2;
        *msg_len = n;
        off_ += 2 + n;
        --remaining_;
        return true;
    }

    bool truncated() const { return truncated_; }
    // Blocks the header promised that the packet did not contain.
    uint16_t unconsumed() const { return remaining_; }

private:
    const uint8_t* p_;
    size_t len_;
    uint16_t remaining_;
    size_t off_;
    bool truncated_ = false;
};

// ---- building packets -------------------------------------------------------

inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xff);
}

inline void write_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
}

inline void write_header(uint8_t* p, const char* session, uint64_t sequence,
                         uint16_t count) {
    std::memset(p, ' ', kSessionLen);
    const size_t n = std::strlen(session);
    std::memcpy(p, session, n < kSessionLen ? n : kSessionLen);
    write_be64(p + kSessionLen, sequence);
    write_be16(p + kSessionLen + 8, count);
}

}  // namespace itchbook::mold
