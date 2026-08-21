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
// The obvious ceiling is arithmetic: if decompression costs D and the book costs
// B, the sequential total is D + B, the overlapped total cannot beat max(D, B),
// and the available speedup is (D + B) / max(D, B) -- at most 2x, and 2x only
// when the halves are balanced.
//
// MEASURED SPEEDUP WENT THROUGH THAT CEILING, at every chunk size, by 21% to
// 30%. Not because a pipeline can beat max(D, B), but because the ceiling
// assumes the WORK IS INVARIANT under the split, and it is not. Two measured
// reasons, both in docs/phase10-results.md:
//
//   * B derived as (sequential - D) overstates the book. Running the same feed
//     uncompressed -- gzread reads a plain file transparently, so it is the same
//     binaries on the same bytes -- puts the book's isolated cost 9% below the
//     subtraction. zlib's window and the book's ref map do not sit in the same
//     cache comfortably, so interleaving them costs more than either alone.
//   * The split moves work OFF the consumer. In the sequential path every
//     message costs two gzread calls and a vector resize; here the producer
//     absorbs those and the consumer walks a contiguous chunk. That is a
//     cheaper inner loop, not overlap, and it lands in the same number.
//
// So the arithmetic is still the right way to think about what overlap can buy,
// and it is not a bound on what this change buys. A measurement above it is the
// signal that the decomposition is leaking -- which is worth more than the
// ceiling was.
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
// THE STALL COUNTERS COUNT POLLS, NOT TIME, AND THAT IS A TRAP. The producer
// counts polls where the ring was full and the consumer counts polls where it
// was empty, which answers "did either side ever wait" and "did the ring ever
// fill". It does NOT support the obvious inference that more producer stalls
// means the book is the slow half, because a poll costs a different amount on
// each side: the consumer's empty poll is a load and a compare, while the
// producer's full poll goes through writable(), which refreshes the consumer's
// cache line whenever it would report zero. The consumer therefore spins many
// more times per second, and the raw counts are not comparable across the
// boundary.
//
// book_replay printed "bottleneck: decompression" from exactly that comparison,
// on a feed whose own timings said the book was the larger half. Which side is
// slower is a question about TIME, and the only sound way to answer it is to
// measure decompression alone, measure the whole run, and subtract -- which is
// what bench/reader-overlap.py does.
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
        // One message's worth of staging, so a message that does not fit in the
        // chunk being filled is carried to the next one rather than re-read.
        alignas(8) uint8_t carry[2 + 65535];
        size_t carry_len = 0;
        try {
            for (;;) {
                while (shared->ring.writable() == 0) {
                    ++shared->stats.producer_stalls;
                }
                Slot& c = shared->ring.write_slot(0);
                c.len = 0;
                // A message read but not yet placed, because it did not fit in
                // the chunk that was being filled. It goes at the front of this
                // one.
                //
                // The first version of this tried to avoid the carry by
                // stopping while a maximum-size message would still fit:
                // `if (c.len + 2 + 65535 > ChunkBytes) break;`. With the
                // default 64 KB chunk that condition is c.len + 65537 > 65536,
                // which is true on the first iteration and every iteration
                // after it -- so the producer broke out before reading a byte,
                // published nothing, never saw EOF, and spun forever. The
                // reservation was larger than the buffer it was reserving from.
                if (carry_len > 0) {
                    // Guaranteed to fit: nothing larger than a chunk is ever
                    // carried, because the read above refuses it outright.
                    if (carry_len > ChunkBytes) {
                        throw std::runtime_error("reader: carried message exceeds the chunk");
                    }
                    std::memcpy(c.bytes, carry, carry_len);
                    c.len = static_cast<uint32_t>(carry_len);
                    carry_len = 0;
                }
                bool eof = false;
                for (;;) {
                    uint8_t lb[2];
                    const int n = gzread(gz, lb, 2);
                    if (n == 0) { eof = true; break; }
                    if (n != 2) throw std::runtime_error("reader: truncated length prefix");
                    const auto len = static_cast<uint16_t>(
                        (static_cast<uint16_t>(lb[0]) << 8) | lb[1]);
                    carry[0] = lb[0];
                    carry[1] = lb[1];
                    const int m = gzread(gz, carry + 2, len);
                    if (m != static_cast<int>(len)) {
                        throw std::runtime_error("reader: truncated message body");
                    }
                    ++shared->stats.messages;
                    shared->stats.bytes += 2u + len;
                    // Refuse it HERE, on the read, not on the placement.
                    //
                    // The first version asked "does it fit in the chunk I am
                    // filling, and is that chunk empty?" -- which is the right
                    // question one message too late. A message that does not
                    // fit alongside what is already staged gets carried to the
                    // next chunk, and the carry was memcpy'd in at the top of
                    // that chunk with no size check at all. A 902-byte message
                    // behind a 50-byte one, with a 256-byte chunk, wrote 902
                    // bytes into 256. ASan caught it; the "identical book"
                    // comparison never could, because real ITCH messages are at
                    // most 50 bytes and a 64 KB chunk never gets near this.
                    //
                    // Whether a message fits is a property of the message and
                    // the chunk size, not of what happens to be staged, so it
                    // is answered once, on the way in.
                    if (2u + len > ChunkBytes) {
                        throw std::runtime_error(
                            "reader: message of " + std::to_string(2 + len) +
                            " bytes exceeds the reader chunk of " +
                            std::to_string(ChunkBytes) + " bytes");
                    }
                    if (c.len + 2u + len > ChunkBytes) {
                        carry_len = 2u + len;      // whole, into the next chunk
                        break;
                    }
                    std::memcpy(c.bytes + c.len, carry, 2u + len);
                    c.len += 2u + len;
                }
                if (c.len > 0) {
                    ++shared->stats.chunks;
                    const size_t occ = shared->ring.size() + 1;
                    if (occ > shared->stats.max_occupancy) shared->stats.max_occupancy = occ;
                    shared->ring.publish(1);
                }
                if (eof && carry_len == 0) break;
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
