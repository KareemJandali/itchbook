#pragma once
//
// sequencer.hpp — deciding whether you have the whole feed.
//
// A UDP multicast feed gives you four things that a file does not, and the
// difference between them is the entire job here:
//
//   * **Loss.** A packet never arrives. The book you build without it is wrong
//     in a way nothing downstream can detect — the messages that did arrive are
//     individually valid, so a parser sees nothing amiss and a book silently
//     diverges from the market for the rest of the day.
//   * **Reordering.** A packet arrives early. This looks exactly like loss at
//     the moment it happens and is not loss at all, so a receiver that treats
//     every out-of-order packet as a gap declares disasters that never
//     occurred and resyncs constantly.
//   * **Duplication.** A packet arrives twice, or a retransmission overlaps
//     what you already have. Applying it twice double-counts every execution
//     in it.
//   * **Silence.** Nothing arrives. Without heartbeats, "the market is quiet"
//     and "my link is down" are the same observation.
//
// Loss and reordering are the interesting pair, because they are
// indistinguishable at the instant a sequence number jumps and require
// opposite responses. The only thing that separates them is waiting: a
// reordered packet turns up shortly, a lost one does not. So this holds
// out-of-order packets in a bounded buffer and only declares a gap once the
// buffer is full or enough packets have gone by — and both of those bounds are
// a policy choice that has to be stated rather than a fact that can be derived.
//
// What it deliberately does NOT do is guess. On a declared gap it reports the
// exact range of missing sequence numbers and hands the decision upstream. A
// receiver that skips a gap and keeps going is the failure mode this phase
// exists to prevent.
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "itchbook/mold/packet.hpp"

namespace itchbook::mold {

struct SequencerConfig {
    // How many out-of-order packets to hold while waiting for a gap to fill.
    // Bigger tolerates worse reordering and costs memory; smaller declares
    // gaps that would have healed. There is no correct value, only a value
    // matched to the path the feed takes.
    size_t reorder_depth = 64;
    // ...and how many packets may go by with the gap still open. A single
    // late packet can otherwise pin the buffer indefinitely on a quiet
    // symbol while the rest of the feed streams past it.
    uint64_t reorder_patience = 256;
};

struct SequencerStats {
    uint64_t packets = 0;           // accepted, including heartbeats
    uint64_t messages = 0;          // delivered downstream
    uint64_t heartbeats = 0;
    uint64_t duplicate_packets = 0; // wholly seen before
    uint64_t duplicate_messages = 0;// individual messages already delivered
    uint64_t reordered_packets = 0; // arrived early, delivered from the buffer
    uint64_t gaps = 0;              // ranges declared permanently lost
    uint64_t messages_lost = 0;
    uint64_t truncated_packets = 0; // header promised more than the bytes held
    uint64_t session_changes = 0;
    uint64_t buffered_now = 0;      // currently held out of order
};

// `Handler` needs:
//   void on_message(char type, const uint8_t* payload, uint16_t len)
//   void on_gap(uint64_t first_missing, uint64_t count)
//
// Templated rather than virtual, matching parser.hpp: the whole path inlines.
template <typename Handler>
class Sequencer {
public:
    Sequencer() = default;
    explicit Sequencer(SequencerConfig cfg) : cfg_(cfg) {}

    void on_packet(const uint8_t* p, size_t len, Handler& h) {
        Header hdr;
        if (!parse_header(p, len, &hdr)) {
            ++stats_.truncated_packets;
            return;
        }
        ++stats_.packets;

        // A new session is a new sequence space. Carrying the old expectation
        // across would declare a gap of however far the previous session got.
        if (!started_ || std::strcmp(session_, hdr.session) != 0) {
            if (started_) ++stats_.session_changes;
            std::memcpy(session_, hdr.session, sizeof(session_));
            expected_ = hdr.sequence;
            buffer_.clear();
            started_ = true;
        }

        if (hdr.end_of_session()) {
            ended_ = true;
            return;
        }
        if (hdr.heartbeat()) {
            ++stats_.heartbeats;
            // A heartbeat's sequence is the next one the SENDER will send. If
            // that is ahead of us, messages were sent that we never received —
            // and on a quiet symbol this is the only thing that will ever tell
            // us so.
            if (hdr.sequence > expected_) {
                declare_gap(hdr.sequence, h);
            }
            return;
        }

        if (hdr.sequence == expected_) {
            deliver(p, len, hdr, 0, h);
            drain(h);
            return;
        }
        if (hdr.sequence < expected_) {
            const uint64_t end = hdr.sequence + hdr.message_count();
            if (end <= expected_) {
                ++stats_.duplicate_packets;
                stats_.duplicate_messages += hdr.message_count();
                return;
            }
            // A partial overlap — a retransmission that starts before where we
            // are. Deliver only the tail. Dropping the whole packet here would
            // lose the messages past `expected_`; applying all of it would
            // replay the ones before.
            const uint64_t already = expected_ - hdr.sequence;
            stats_.duplicate_messages += already;
            deliver(p, len, hdr, already, h);
            drain(h);
            return;
        }

        // Ahead of us: loss or reordering, and we cannot yet tell which.
        buffer_packet(p, len, hdr, h);
    }

    // The stream ended. Anything still buffered was never reachable, and a gap
    // that is still open at end-of-day is still a gap — reporting it here is
    // what stops a quiet failure from finishing the session looking clean.
    void flush(Handler& h) {
        while (!buffer_.empty()) {
            declare_gap(buffer_.begin()->first, h);
            drain(h);
        }
    }

    const SequencerStats& stats() const {
        stats_.buffered_now = buffer_.size();
        return stats_;
    }
    uint64_t expected() const { return expected_; }
    bool ended() const { return ended_; }
    const char* session() const { return session_; }

private:
    void deliver(const uint8_t* p, size_t len, const Header& hdr, uint64_t skip,
                 Handler& h) {
        BlockIterator it(p, len, hdr.message_count());
        const uint8_t* msg = nullptr;
        uint16_t msg_len = 0;
        uint64_t index = 0;
        while (it.next(&msg, &msg_len)) {
            if (index++ < skip) continue;
            if (msg_len > 0) {
                h.on_message(static_cast<char>(msg[0]), msg, msg_len);
                ++stats_.messages;
            }
            ++expected_;
        }
        if (it.truncated()) {
            ++stats_.truncated_packets;
            // The blocks we could not read are missing bytes, not missing
            // sequence numbers we can wait for. Treat them as lost now rather
            // than leaving `expected_` short and manufacturing a phantom gap
            // on the next packet.
            const uint64_t missing = it.unconsumed();
            if (missing > 0) {
                ++stats_.gaps;
                stats_.messages_lost += missing;
                h.on_gap(expected_, missing);
                expected_ += missing;
            }
        }
    }

    void buffer_packet(const uint8_t* p, size_t len, const Header& hdr, Handler& h) {
        auto found = buffer_.find(hdr.sequence);
        if (found == buffer_.end()) {
            buffer_.emplace(hdr.sequence, std::vector<uint8_t>(p, p + len));
        } else {
            ++stats_.duplicate_packets;
        }
        ++since_gap_;
        if (buffer_.size() >= cfg_.reorder_depth || since_gap_ > cfg_.reorder_patience) {
            declare_gap(buffer_.begin()->first, h);
            drain(h);
        }
    }

    // Everything from `expected_` up to `up_to` is gone. Say so, then resume
    // from there.
    void declare_gap(uint64_t up_to, Handler& h) {
        if (up_to <= expected_) return;
        const uint64_t missing = up_to - expected_;
        ++stats_.gaps;
        stats_.messages_lost += missing;
        h.on_gap(expected_, missing);
        expected_ = up_to;
        since_gap_ = 0;
    }

    // Deliver whatever the buffer now allows, in order.
    void drain(Handler& h) {
        for (;;) {
            auto it = buffer_.begin();
            if (it == buffer_.end()) break;
            Header hdr;
            if (!parse_header(it->second.data(), it->second.size(), &hdr)) {
                buffer_.erase(it);
                continue;
            }
            const uint64_t end = hdr.sequence + hdr.message_count();
            if (end <= expected_) {          // wholly superseded while it waited
                ++stats_.duplicate_packets;
                buffer_.erase(it);
                continue;
            }
            if (hdr.sequence > expected_) break;   // still a hole in front of it
            const uint64_t already = expected_ - hdr.sequence;
            std::vector<uint8_t> pkt = std::move(it->second);
            buffer_.erase(it);
            ++stats_.reordered_packets;
            deliver(pkt.data(), pkt.size(), hdr, already, h);
        }
        if (buffer_.empty()) since_gap_ = 0;
    }

    SequencerConfig cfg_;
    mutable SequencerStats stats_;
    std::map<uint64_t, std::vector<uint8_t>> buffer_;
    char session_[kSessionLen + 1] = {};
    uint64_t expected_ = 0;
    uint64_t since_gap_ = 0;
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace itchbook::mold
