#pragma once
//
// trace.hpp — where phase 12.8's timestamps live between being taken and being
// written out.
//
// The rule this file exists to obey is rdtsc.hpp's opening one: never allocate,
// never print, never touch a stream inside the region being measured, because
// "a single malloc is worth more cycles than the thing you are trying to
// observe." Everything here is sized before the process prints READY and is
// never resized afterwards.
//
// TWO CHAINS AND ONE INTERVAL, NOT SEVEN HOPS. docs/phase12-8-design.md §2 sets
// out why the plan's single seven-hop chain does not survive: the interval
// between an order being accepted and being filled is how long the quote sat in
// the book waiting for the historical tape to reach its price, which is market
// structure divided by the replay multiplier and is not a latency at all. So
// chain A runs arrival -> book -> decision -> write -> accept, chain B runs
// aggressor -> match -> pack -> wire -> recognised, and the resting interval
// between them is reported in replay seconds on its own axis.
//
// WHAT AN OVERFLOWING ARENA MUST NOT DO. It must not wrap. A wrapped index
// silently pairs one order's t0 with another order's t3, and the resulting
// value looks exactly like a plausible latency -- there is nothing about it a
// reader could notice. So an index past the end increments dropped_samples,
// records nothing, and the run refuses to publish. `reserve_exceeded` is
// reported the way tools/wire_to_book.cpp already reports its own reallocation,
// because a histogram whose backing store grew inside the measured region is
// describing the growth as much as the pipeline.
//
// COUNTS, NOT BOOLEANS. Every hop carries a stamps_taken counter that must be
// non-zero and must match the population it claims to cover. wire_to_book
// closed its socket before reading /proc/net/udp, so kernel_drops returned 0 on
// every run ever made and the exit code that depended on it was unreachable --
// a gate whose input was a constant. A field `instrumented: true` is the same
// defect wearing a different type.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace itchbook::bench {

// ---- chain A, one per order the strategy sends -------------------------------
//
// Indexed directly by the token's decimal suffix, which tools/strategy.cpp
// already mints as a dense counter from 1 -- so this is an array subscript and
// never a hash lookup.
struct ChainA {
    uint64_t t0 = 0;        // recvfrom returned, for the datagram that armed it
    uint64_t t1 = 0;        // that datagram's trigger message applied
    uint64_t t1p = 0;       // quote block entered, i.e. the drain has finished
    uint64_t t2 = 0;        // price decided and non-zero
    uint64_t t3_pre = 0;    // immediately BEFORE that write's ::write() call
    uint64_t t3 = 0;        // the write that carried this order's frame
    // t3 is stamped AFTER ::write() returns, and Linux delivers loopback
    // synchronously in the sender's own call stack: a busy-polling reader on
    // the other core can observe the bytes and stamp its own receipt before
    // write() has returned here. t3 therefore OVERSHOOTS the instant the bytes
    // became visible, and t3 -> t3' came out negative for 27.2% of 12,000
    // orders on bare metal -- never by more than (t3 - t2), which is the tell.
    // The true send instant lies in (t3_pre, t3); neither endpoint is it. The
    // transport hop is an INTERVAL [t3' - t3, t3' - t3_pre], and pinning it
    // tighter needs a stamp inside the kernel, which this harness does not
    // have. The UDP side is stamped before ::sendto and has no such problem:
    // 0 negatives in 20,841 fills.
    uint64_t iter_start = 0;   // top of the poll iteration that produced it
    uint64_t iter_end = 0;     // bottom of the same iteration
    uint64_t tsc0 = 0;      // paired rdtscp at t0, second instrument
    uint64_t tsc3 = 0;      // paired rdtscp at t3
    // Thread CPU time bracketing the headline hop, for the gap-overlap census:
    // (t3 - t1p) - (cpu_t3 - cpu_t1p) is the time this thread was NOT RUNNING
    // inside the interval. Read outside the interval, so the bias is toward
    // under-tagging and is bounded by the reads' own cost.
    uint64_t cpu_t1p = 0;
    uint64_t cpu_t3 = 0;
    uint32_t ref_seq = 0;   // from the OUCH Accepted; 0 until it lands
    uint32_t stride = 0;    // messages actually applied since the last quote
    uint32_t dgrams_after_trigger = 0;
    uint32_t msgs_after_trigger = 0;
    uint16_t cpu0 = 0xFFFF; // from rdtscp aux; disagreement means migration
    uint16_t cpu3 = 0xFFFF;
    uint8_t  resp = 0;      // 'A' accepted, 'J' rejected, 0 nothing yet
    uint8_t  have = 0;      // bit per stamp; see kHave*
    uint8_t  terminal = 0;  // see Terminal
    uint8_t  pad = 0;
};

enum : uint8_t {
    kHaveT0 = 1u << 0, kHaveT1 = 1u << 1, kHaveT1p = 1u << 2,
    kHaveT2 = 1u << 3, kHaveT3 = 1u << 4,
};

// How each order ended. The census over these must sum to orders_sent, or the
// run is refused: a chain that quietly disappears is a chain whose absence
// could be the interesting thing.
enum Terminal : uint8_t {
    kTermUnknown = 0,
    kTermFilledMaker,       // rested and was named on the tape
    kTermRestedUnfilled,    // still in the book when the run ended
    kTermCrossed,           // traded on entry; ITCH never names a taker
    kTermRejected,
    kTermInFlight,          // sent, nothing back before the run ended
};

// ---- chain B, one per maker fill ---------------------------------------------
struct FillRec {
    uint64_t t6p = 0;       // recvfrom returned for the datagram carrying the 'E'
    uint64_t t6 = 0;        // the 'E' recognised as ours
    uint32_t ref_seq = 0;
    uint32_t fill_ordinal = 0;   // n-th fill of THIS reference; see below
    uint32_t shares = 0;
    uint8_t  in_book = 0;   // was it resting in our own book at t6
    uint8_t  parked = 0;    // recognised before its acknowledgement arrived
    uint16_t pad = 0;
};

// ---- the exchange side --------------------------------------------------------
struct AcceptEx {
    uint64_t t3p = 0;       // the read that completed this order's frame
    uint64_t t4 = 0;        // the first OUCH response for it
    uint32_t token_seq = 0;
    uint8_t  resp = 0;
    uint8_t  pad[3] = {};
};

struct EmitEx {
    uint64_t tA = 0;        // the historical 'E' left the reader
    uint64_t t5a = 0;       // apply_external_fill returned; the fill exists
    uint64_t mold_seq = 0;  // sequence this 'E' will carry, for the packet join
    uint32_t ref_seq = 0;
    uint32_t fill_ordinal = 0;
};

struct PktEx {
    uint64_t header_seq = 0;   // sequence of the FIRST message in the datagram
    uint64_t t5b = 0;          // sendto
    uint32_t header_count = 0;
    uint32_t pad = 0;
};

// ---- a fixed arena ------------------------------------------------------------
//
// Append-only, never grows. Overflow is counted and dropped, never wrapped:
// see the banner.
template <typename T>
class Arena {
public:
    void reserve(size_t n) { store_.assign(n, T{}); cap_ = n; used_ = 0; }

    // Append. Returns nullptr when full, having counted the loss.
    T* push() {
        if (used_ >= cap_) { ++dropped_; return nullptr; }
        T* p = &store_[used_++];
        if (used_ > high_) high_ = used_;
        return p;
    }

    // Direct index, for the arenas keyed by a dense counter.
    T* at(size_t i) {
        if (i >= cap_) { ++dropped_; return nullptr; }
        if (i + 1 > high_) high_ = i + 1;
        return &store_[i];
    }

    const T* data() const { return store_.data(); }
    size_t size() const { return used_; }
    size_t capacity() const { return cap_; }
    size_t high_water() const { return high_; }
    uint64_t dropped() const { return dropped_; }
    bool reserve_exceeded() const { return dropped_ != 0; }

    // For the direct-indexed arenas, `used_` never advances; the population is
    // the high-water mark instead.
    void note_used(size_t n) { if (n > used_) used_ = n; }

private:
    std::vector<T> store_;
    size_t cap_ = 0;
    size_t used_ = 0;
    size_t high_ = 0;
    uint64_t dropped_ = 0;
};

// ---- per-hop stamp counts ------------------------------------------------------
//
// One per hop, and every one must be non-zero before a number is published.
struct StampCounts {
    uint64_t t0 = 0, t1 = 0, t1p = 0, t2 = 0, t3 = 0;
    uint64_t t3p = 0, t4 = 0, tA = 0, t5a = 0, t5b = 0, t6p = 0, t6 = 0;
    uint64_t migrations = 0;      // paired rdtscp stamps that disagreed on CPU
    uint64_t negative_intra = 0;  // a harness bug; the run aborts on these
};

// ---- writing it out ------------------------------------------------------------
//
// Raw records first, as the first action after the loop and before any sorting
// or formatting, so a crash in post-processing costs the analysis and not the
// data. Fixed-width little-endian structs with a magic and a version, because
// the reader is a Python script and a silent layout change would be read as
// plausible numbers.
inline constexpr uint32_t kTraceMagic = 0x38324254;   // "TB28"
inline constexpr uint32_t kTraceVersion = 2;   // 2: ChainA gained t3_pre

template <typename T>
inline bool write_section(std::FILE* f, const char tag[4], const Arena<T>& a,
                          size_t count) {
    uint32_t n = static_cast<uint32_t>(count);
    uint32_t rec = static_cast<uint32_t>(sizeof(T));
    if (std::fwrite(tag, 1, 4, f) != 4) return false;
    if (std::fwrite(&n, sizeof(n), 1, f) != 1) return false;
    if (std::fwrite(&rec, sizeof(rec), 1, f) != 1) return false;
    if (n != 0 && std::fwrite(a.data(), sizeof(T), n, f) != n) return false;
    return true;
}

inline std::FILE* trace_open(const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return nullptr;
    uint32_t m = kTraceMagic;
    uint32_t v = kTraceVersion;
    std::fwrite(&m, sizeof(m), 1, f);
    std::fwrite(&v, sizeof(v), 1, f);
    return f;
}

}  // namespace itchbook::bench
