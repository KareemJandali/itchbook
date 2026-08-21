#pragma once
//
// spsc_ring.hpp — one producer, one consumer, no locks.
//
// Hand-written, and not because a library one would be worse. Every line here
// has to be defensible out loud, and a dependency you can only use is not the
// same as a structure you can explain. The comments below are the explanation,
// written where the code is rather than in a document that will drift from it.
//
// The shape: a fixed array of slots, two indices, and a memory-ordering
// argument that makes the whole thing safe with no mutex, no CAS, and no
// atomic read-modify-write anywhere. Both indices are written by exactly one
// thread each, which is what SPSC buys and what makes plain stores legal where
// a multi-producer queue would need a CAS loop.
//
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

namespace itchbook::pipe {

// A cache line. std::hardware_destructive_interference_size is the standard
// spelling and is not available everywhere; 64 is right on every x86-64 and on
// Apple silicon's 128-byte lines it is merely conservative, which is the safe
// direction to be wrong in.
inline constexpr size_t kCacheLine = 64;

template <typename Slot, size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "a ring of one slot is a variable");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // ---- indices ------------------------------------------------------------
    //
    // head_ and tail_ COUNT, they do not point. They increase monotonically for
    // the life of the ring and are masked only when a slot is addressed.
    //
    // The alternative — wrapping the indices themselves — forces a wasted slot
    // or a separate count, because with wrapped indices `head == tail` means
    // both empty and full and the two cannot be told apart. Counting instead:
    //
    //     empty   head == tail
    //     full    head - tail == Capacity
    //     size    head - tail                      (never needs a lock)
    //
    // Overflow of the counter itself: at ten million messages a second, a
    // uint64 lasts about 58,000 years. Worth one sentence and no code.
    //
    // Unsigned subtraction is also what makes `head - tail` correct without a
    // branch even in the (unreachable) case where the counter wrapped: modular
    // arithmetic gives the right distance as long as the ring is smaller than
    // half the counter's range, which at 2^64 it comfortably is.

    // ---- memory ordering ----------------------------------------------------
    //
    // The producer writes a slot, then publishes head_ with a RELEASE store.
    // The consumer reads head_ with an ACQUIRE load, then reads the slot.
    //
    // That pair is the entire safety argument, and it is worth being precise
    // about what it buys. The release store guarantees that every write the
    // producer made BEFORE it — including the slot — is visible to any thread
    // that sees the released value through an acquire load. Without it, the
    // compiler or the CPU is free to publish the index before the payload, and
    // the consumer reads a slot that has been announced but not yet written.
    // Nothing about that failure is rare or exotic: it is a store-store
    // reordering, and it is legal.
    //
    // Why not relaxed on the index publish? Relaxed gives atomicity — the load
    // sees some whole value, never a torn one — and NOTHING about ordering. The
    // consumer could see the new index and the old slot contents. Atomicity was
    // never the problem; the happens-before edge is.
    //
    // Why not seq_cst? It adds a single total order across ALL seq_cst
    // operations in the program, which this algorithm does not use: correctness
    // here needs one edge from producer to consumer, not agreement between
    // unrelated threads about the order of unrelated operations. On x86-64 a
    // seq_cst store compiles to XCHG or MOV+MFENCE instead of a plain MOV, so
    // it costs a full barrier on the hottest line for a guarantee nothing reads.
    //
    // The consumer's tail_ store is release for the mirror-image reason: the
    // producer must not overwrite a slot until the consumer's reads of it are
    // done, and release-on-tail is what orders those reads before the release.

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    static constexpr size_t capacity() { return Capacity; }

    // ---- producer side ------------------------------------------------------

    // A LOWER BOUND on the slots available to write. Never more than the truth;
    // sometimes fewer.
    //
    // That asymmetry is the contract, and it is worth stating because the
    // obvious reading of the name is wrong. Pushing does not refresh the
    // producer's view of the consumer, so after a burst of writes this can
    // report less room than really exists — and it refreshes only when it would
    // otherwise report none. The intended loop is therefore
    //
    //     size_t n = ring.writable();   // ask once
    //     fill n slots; ring.publish(n);
    //
    // which asks the consumer's cache line for permission once per batch rather
    // than once per message. A caller that wants the true figure calls this
    // until it is non-zero, or reads size(); a caller that treats a small answer
    // as "the ring is nearly full" has misread it.
    //
    // The cached copy is the optimisation that matters most and the one that is
    // easiest to get wrong. tail_ is written by the CONSUMER, so reading it
    // bounces its cache line to this core every time — on a ring that is rarely
    // full, that is a cross-core transfer per message to learn something the
    // producer already knew. So the producer keeps a stale copy and only
    // re-reads the real one when the stale copy says there is no room. The
    // staleness is always in the safe direction: cached_tail_ can only be
    // BEHIND the truth, which makes the ring look fuller than it is, which
    // costs a re-read and never a lost message.
    size_t writable() {
        const uint64_t head = head_.load(std::memory_order_relaxed);   // ours
        size_t free_slots = Capacity - static_cast<size_t>(head - cached_tail_);
        if (free_slots == 0) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            free_slots = Capacity - static_cast<size_t>(head - cached_tail_);
        }
        return free_slots;
    }

    // The slot `n` ahead of the write cursor, for filling in place. Writing a
    // batch means calling this for each slot and then publish(n) ONCE.
    Slot& write_slot(size_t n = 0) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        return slots_[(head + n) & kMask];
    }

    // Make `n` written slots visible. One release store per batch, not per
    // message: a whole recvmmsg batch of 32 costs one publish instead of 32,
    // and the release still orders every one of the 32 slot writes before it.
    //
    // Precondition: `n` was obtained from writable() and no more. Publishing
    // more than that overruns slots the consumer has not finished with, and the
    // symptom is not a crash — it is head_ - tail_ exceeding Capacity, after
    // which every count in the structure is nonsense and the underflow surfaces
    // somewhere else entirely. The assert costs nothing in Release and turns a
    // silent corruption into a line number in Debug, which is where the tests
    // run. It found this by being written after a test violated it.
    void publish(size_t n) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        assert(head + n - tail_.load(std::memory_order_relaxed) <= Capacity &&
               "published more slots than writable() offered");
        head_.store(head + n, std::memory_order_release);
    }

    // Single-message convenience. Returns false when the ring is full — it does
    // not block, does not spin, and does not overwrite. What to do about a full
    // ring is a policy question that belongs to the caller: in this project the
    // receiver drops the packet and counts it, which turns backpressure into a
    // sequence gap that phase 7's machinery already knows how to grade.
    bool push(const Slot& value) {
        if (writable() == 0) return false;
        write_slot(0) = value;
        publish(1);
        return true;
    }

    // ---- consumer side ------------------------------------------------------

    // A LOWER BOUND on the slots available to read. Mirror of writable(), with
    // the same contract and the same reason: cached_head_ can only be behind,
    // which makes the ring look emptier than it is, which costs a re-read and
    // never a duplicated message. Drain what it reports, then ask again.
    size_t readable() {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);   // ours
        size_t ready = static_cast<size_t>(cached_head_ - tail);
        if (ready == 0) {
            cached_head_ = head_.load(std::memory_order_acquire);
            ready = static_cast<size_t>(cached_head_ - tail);
        }
        return ready;
    }

    const Slot& read_slot(size_t n = 0) const {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        return slots_[(tail + n) & kMask];
    }

    // Release `n` consumed slots back to the producer.
    //
    // Precondition, and the mirror of publish()'s: `n` came from readable().
    // Consuming more moves tail_ past head_, and since the counts are unsigned
    // the next readable() computes a vast positive number rather than a
    // negative one — the ring reports billions of messages waiting in an empty
    // buffer. That is exactly how it failed the first time a test consumed more
    // than it was offered.
    void consume(size_t n) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        assert(tail + n <= head_.load(std::memory_order_relaxed) &&
               "consumed more slots than readable() offered");
        tail_.store(tail + n, std::memory_order_release);
    }

    bool pop(Slot* out) {
        if (readable() == 0) return false;
        *out = read_slot(0);
        consume(1);
        return true;
    }

    // ---- observation --------------------------------------------------------
    //
    // Both loads are relaxed and the answer is a snapshot of a moving target,
    // which is all a monitor can ever have. The kill switch watches occupancy
    // to decide whether the system is drowning; it does not need a consistent
    // read, it needs a cheap one.
    size_t size() const {
        return static_cast<size_t>(head_.load(std::memory_order_relaxed) -
                                   tail_.load(std::memory_order_relaxed));
    }
    bool empty() const { return size() == 0; }
    bool full() const { return size() == Capacity; }

    uint64_t produced() const { return head_.load(std::memory_order_relaxed); }
    uint64_t consumed() const { return tail_.load(std::memory_order_relaxed); }

private:
    static constexpr uint64_t kMask = Capacity - 1;

    // ---- false sharing ------------------------------------------------------
    //
    // Each index gets its own cache line, and this is the first thing a reviewer
    // looks for. Two atomics sharing a line means every producer publish
    // invalidates the line the consumer is reading tail_ from and vice versa —
    // the two threads ping-pong one line between cores at message rate, and the
    // structure performs like a contended lock while containing none.
    //
    // The cached copies are separated too, and that is less obvious: they are
    // plain non-atomic members, but cached_tail_ is written by the producer on
    // every re-read and cached_head_ by the consumer, so putting them on the
    // same line as each other would reintroduce exactly the bouncing the cache
    // exists to avoid.
    //
    // Predicted flat and worth testing anyway: padding to 128 bytes for
    // adjacent-line prefetchers, which on some cores fetch line pairs and can
    // make 64-byte separation insufficient. Apple silicon has 128-byte lines,
    // so on this machine the prediction may not survive. See bench/.
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};   // producer writes
    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};   // consumer writes
    alignas(kCacheLine) uint64_t cached_tail_{0};         // producer's stale copy
    alignas(kCacheLine) uint64_t cached_head_{0};         // consumer's stale copy
    alignas(kCacheLine) std::array<Slot, Capacity> slots_{};
};

}  // namespace itchbook::pipe
