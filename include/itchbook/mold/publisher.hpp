#pragma once
//
// publisher.hpp — the sending half of MoldUDP64.
//
// sequencer.hpp is receive-side only: on_packet() and flush() both consume.
// The sending half existed exactly once, inline in tools/mold_wrap.cpp's
// main(), where it wrote to a gzip file. Phase 12.7 needs the same packing
// against a UDP socket, in a process that is also running a matcher, so it
// moves here rather than being written a second time — a second copy is a
// second thing to keep in step with the sequencer that has to parse it.
//
// What it owns: batching messages into a packet up to an MTU, the sequence
// arithmetic (a packet's header carries the sequence of its FIRST message, and
// the next packet's is that plus the count), heartbeats, and the end-of-session
// marker. What it does not own: the transport. Bytes leave through a callback,
// the same shape emit::Sink uses, so a file, a socket and a test buffer are all
// callers rather than cases.
//
// ONE RULE THAT DOES NOT CARRY ACROSS FROM mold_wrap.cpp. There, heartbeats are
// paced by the FEED's own timestamps, because the file has no other clock and a
// heartbeat's job in a recorded stream is to make a quiet period legible. Here
// there are two clocks and the choice matters: a heartbeat exists so a receiver
// can tell "nothing happened" from "the link died", which is a WALL-clock
// question. Pacing them on replay time would emit a burst of heartbeats during
// a fast replay and none at all during a slow one — exactly backwards. So
// heartbeat pacing is driven by the caller through maybe_heartbeat(wall_ns),
// while every message's own timestamp stays replay time.
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/mold/packet.hpp"

namespace itchbook::mold {

class Publisher {
public:
    // `send` receives one complete datagram. Returning is the only contract;
    // a caller that drops it is modelling a lossy link, which is a legitimate
    // thing to want.
    using Send = void (*)(void* ctx, const uint8_t* datagram, size_t len);

    Publisher(std::string session, size_t mtu, Send send, void* ctx)
        : session_(std::move(session)), mtu_(mtu), send_(send), ctx_(ctx),
          packet_(mtu) {}

    // Add one ITCH message. Flushes first if it would not fit, so a datagram
    // never splits a message: MoldUDP64 block framing has no continuation.
    // Returns false only if the message could never fit alone, which is a
    // configuration error rather than a runtime condition.
    bool add(const uint8_t* msg, size_t len) {
        const size_t need = 2 + len;
        // The subtraction below is on size_t. An mtu smaller than the header
        // wraps it to something enormous, the fit check passes, and the write
        // lands past the end of a buffer sized to that same mtu.
        if (mtu_ <= kHeaderLen || need > mtu_ - kHeaderLen) return false;
        if (used_ + need > mtu_ || count_ == kEndOfSession - 1) flush();
        write_be16(packet_.data() + used_, static_cast<uint16_t>(len));
        std::memcpy(packet_.data() + used_ + 2, msg, len);
        used_ += need;
        ++count_;
        ++messages_;
        return true;
    }

    void flush() {
        if (count_ == 0) return;
        write_header(packet_.data(), session_.c_str(), sequence_, count_);
        send_(ctx_, packet_.data(), used_);
        sequence_ += count_;
        used_ = kHeaderLen;
        count_ = 0;
        ++packets_;
    }

    // A heartbeat carries the sequence of the next message that WILL be sent,
    // which is what lets a receiver distinguish a quiet feed from a dead one.
    // Paced on wall clock — see the banner.
    void maybe_heartbeat(uint64_t wall_ns, uint64_t period_ns) {
        if (period_ns == 0) return;
        if (last_hb_ns_ == 0) { last_hb_ns_ = wall_ns; return; }
        if (wall_ns - last_hb_ns_ < period_ns) return;
        flush();
        uint8_t hb[kHeaderLen];
        write_header(hb, session_.c_str(), sequence_, 0);
        send_(ctx_, hb, sizeof(hb));
        last_hb_ns_ = wall_ns;
        ++heartbeats_;
    }

    // Count 0xFFFF, carrying no messages. A receiver reading the count as a
    // plain integer walks off the end of this packet, which is why it is a
    // named constant rather than a literal at the call site.
    void end_of_session() {
        flush();
        uint8_t eos[kHeaderLen];
        write_header(eos, session_.c_str(), sequence_, kEndOfSession);
        send_(ctx_, eos, sizeof(eos));
        ++packets_;
        ended_ = true;
    }

    uint64_t packets() const { return packets_; }
    uint64_t messages() const { return messages_; }
    uint64_t heartbeats() const { return heartbeats_; }
    uint64_t next_sequence() const { return sequence_; }

    // The sequence the NEXT added message will carry. Stable across a flush --
    // flush advances sequence_ by count_ and zeroes count_, so the sum does not
    // move -- which is what lets a caller read it before add() without knowing
    // whether the message is about to start a new packet. Phase 12.8 uses it to
    // join a fill's match stamp to the datagram that eventually carried it.
    uint64_t sequence_of_next_add() const { return sequence_ + count_; }
    bool ended() const { return ended_; }

private:
    std::string session_;
    size_t mtu_;
    Send send_;
    void* ctx_;
    std::vector<uint8_t> packet_;
    size_t used_ = kHeaderLen;
    uint16_t count_ = 0;
    uint64_t sequence_ = 1;          // MoldUDP64 sequences start at 1
    uint64_t messages_ = 0;
    uint64_t packets_ = 0;
    uint64_t heartbeats_ = 0;
    uint64_t last_hb_ns_ = 0;
    bool ended_ = false;
};

}  // namespace itchbook::mold
