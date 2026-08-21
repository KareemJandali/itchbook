#pragma once
//
// reader_thread.hpp — decompress on one core while the book runs on another.
//
// THE GAP THIS CLOSES. Phase 9 reported a full trading day end to end and then
// reported, honestly and repeatedly, that a large part of that number was gzip.
// `itch_census` exists partly to measure it: a pass that decompresses, frames
// and length-checks the file and builds nothing. Its wall clock is the floor
// under any replay of the same file, and the gap between the two is what the
// book itself costs.
//
// Those two costs are strictly sequential today. `parse()` calls
// `Reader::next()`, which calls `gzread`, which inflates; then the handler runs;
// then the next `gzread`. One thread, alternating, so the total is the SUM of
// two things that have no reason to wait for each other. Phase 10 built a
// lock-free ring precisely so two stages could overlap, and the reader path is
// the second stage in this repository that wants one.
//
// The ceiling is arithmetic and worth stating before any measurement: if
// decompression costs D and the book costs B, the sequential total is D + B and
// the overlapped total cannot beat max(D, B). The speedup available is
// (D + B) / max(D, B), which is at most 2x and reaches it only when the two
// halves are exactly balanced. Anyone expecting more has mistaken a pipeline
// for a parallel algorithm.
//
// THE SLOT IS A CHUNK, NOT A MESSAGE. wire_to_book puts one message in a slot
// because a message is what arrives from a socket. Here the producer is reading
// a file and the natural unit is a buffer: one publish per 64 KB rather than
// one per 40 bytes turns the ring's release store from a per-message cost into
// a rounding error. Chunks always break BETWEEN messages -- the producer stops
// filling when the next message would not fit -- so the consumer never has to
// reassemble a message that straddles two slots, which is the bug this design
// exists to make impossible rather than to handle.
//
// WHICH SIDE IS THE BOTTLENECK IS AN OUTPUT. The producer counts the polls
// where the ring was full and the consumer counts the polls where it was empty.
// A run whose producer stalls is a run where the book is the slow half; a run
// whose consumer stalls is decompression-bound. Without those two counters
// "overlap did not help" and "overlap helped and the other half got slower" are
// the same observation, and phase 9's whole story is about not being able to
// tell those apart.
//
#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "itchbook/itch/messages.hpp"
#include "itchbook/pipe/spsc_ring.hpp"

namespace itchbook::pipe {

inline constexpr size_t kReaderChunkBytes = 64 * 1024;
inline constexpr size_t kReaderSlots = 16;

struct ReaderStats {
    uint64_t messages = 0;
    uint64_t bytes = 0;              // framed bytes, matching Reader::bytes()
    uint64_t chunks = 0;
    uint64_t producer_stalls = 0;    // ring full: the BOOK is the slow half
    uint64_t consumer_stalls = 0;    // ring empty: DECOMPRESSION is
    uint64_t max_occupancy = 0;
};

namespace detail {

template <size_t ChunkBytes>
struct Chunk {
    uint32_t len = 0;
    uint8_t bytes[ChunkBytes];
};

}  // namespace detail

// Same contract as itchbook::parse(): the handler needs
// `on_message(char, const uint8_t*, uint16_t)`, the length prefix must agree
// with the spec length where one is known, and a truncated frame throws.
//
// The throw is the part that needed care. The producer runs on another thread,
// where an exception would call std::terminate rather than reach the caller, so
// it is captured and rethrown here -- on the caller's thread, after the join,
// exactly as the single-threaded version would have thrown it. A reader thread
// that turned a desync into a crash would be a worse tool than the one it
// replaces.
template <typename Handler, size_t ChunkBytes = kReaderChunkBytes,
          size_t Slots = kReaderSlots>
uint64_t parse_threaded(const std::string& path, Handler& handler,
                        ReaderStats* out = nullptr) {
    using Slot = detail::Chunk<ChunkBytes>;
    struct Shared {
        SpscRing<Slot, Slots> ring;
        std::atomic<bool> done{false};
        std::exception_ptr error;
        ReaderStats stats;
    };
    // Slots x 64 KB on the heap, not the stack: the default 16 slots is a
    // megabyte, and the parameters exist to be swept.
    auto shared = std::make_unique<Shared>();

    gzFile gz = gzopen(path.c_str(), "rb");
    if (gz == nullptr) throw std::runtime_error("parse_threaded: cannot open " + path);
    gzbuffer(gz, 1u << 20);          // matches Reader, so the comparison is fair

    std::thread producer([&] {
        try {
            for (;;) {
                while (shared->ring.writable() == 0) {
                    ++shared->stats.producer_stalls;
                }
                Slot& c = shared->ring.write_slot(0);
                c.len = 0;
                bool eof = false;
                for (;;) {
                    // Stop while a maximum-size message would still fit, so the
                    // read below never has to be undone. Messages break between
                    // slots by construction.
                    if (c.len + 2 + 65535 > ChunkBytes) break;
                    uint8_t lb[2];
                    const int n = gzread(gz, lb, 2);
                    if (n == 0) { eof = true; break; }
                    if (n != 2) throw std::runtime_error("reader: truncated length prefix");
                    const auto len = static_cast<uint16_t>(
                        (static_cast<uint16_t>(lb[0]) << 8) | lb[1]);
                    c.bytes[c.len] = lb[0];
                    c.bytes[c.len + 1] = lb[1];
                    const int m = gzread(gz, c.bytes + c.len + 2, len);
                    if (m != static_cast<int>(len)) {
                        throw std::runtime_error("reader: truncated message body");
                    }
                    c.len += 2u + len;
                    ++shared->stats.messages;
                    shared->stats.bytes += 2u + len;
                }
                if (c.len > 0) {
                    ++shared->stats.chunks;
                    const size_t occ = shared->ring.size() + 1;
                    if (occ > shared->stats.max_occupancy) shared->stats.max_occupancy = occ;
                    shared->ring.publish(1);
                }
                if (eof) break;
            }
        } catch (...) {
            shared->error = std::current_exception();
        }
        shared->done.store(true, std::memory_order_release);
    });

    uint64_t count = 0;
    std::exception_ptr consumer_error;
    try {
        for (;;) {
            const size_t ready = shared->ring.readable();
            if (ready == 0) {
                if (shared->done.load(std::memory_order_acquire) &&
                    shared->ring.readable() == 0) {
                    break;
                }
                ++shared->stats.consumer_stalls;
                continue;
            }
            for (size_t k = 0; k < ready; ++k) {
                const Slot& c = shared->ring.read_slot(k);
                size_t o = 0;
                while (o + 2 <= c.len) {
                    const auto len = static_cast<uint16_t>(
                        (static_cast<uint16_t>(c.bytes[o]) << 8) | c.bytes[o + 1]);
                    o += 2;
                    const char type = static_cast<char>(c.bytes[o]);
                    const int expected = itch::spec_length(type);
                    if (expected > 0 && len != expected) {
                        throw std::runtime_error(
                            std::string("parser: length mismatch for type '") + type +
                            "' (got " + std::to_string(len) + ", expected " +
                            std::to_string(expected) + ")");
                    }
                    handler.on_message(type, c.bytes + o, len);
                    ++count;
                    o += len;
                }
            }
            shared->ring.consume(ready);
        }
    } catch (...) {
        consumer_error = std::current_exception();
        // Drain rather than abandon: the producer is blocked on a full ring and
        // would spin forever on a thread nobody is going to join.
        while (!shared->done.load(std::memory_order_acquire)) {
            const size_t n = shared->ring.readable();
            if (n > 0) shared->ring.consume(n);
        }
        const size_t n = shared->ring.readable();
        if (n > 0) shared->ring.consume(n);
    }

    producer.join();
    gzclose(gz);
    if (out != nullptr) *out = shared->stats;
    if (consumer_error) std::rethrow_exception(consumer_error);
    if (shared->error) std::rethrow_exception(shared->error);
    return count;
}

}  // namespace itchbook::pipe
