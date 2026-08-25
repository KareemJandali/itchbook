#pragma once
//
// pool.hpp — slab allocator for Order nodes.
//
// Never `new` an Order on the hot path. Orders come from chunks allocated ahead
// of time and handed out from a free list; allocation is a pointer pop and
// deallocation a pointer push.
//
// Chunks are never reallocated, so every Order* stays valid for the life of the
// pool. That matters: the price levels hold raw pointers into this storage, and
// a std::vector<Order> that grew would dangle every one of them.
//
// Chunk sizes grow geometrically rather than being one big fixed slab, and that
// choice is worth more than anything else measured in phase 4. A single 2^20
// chunk is 42MB; allocating and first-touching it costs ~53M cycles, which on a
// one-million-message replay is ~53 cycles/msg amortised — more than the entire
// steady-state cost of handling a message. Starting small and doubling keeps
// the first allocation trivial while holding the chunk count at O(log n), so
// the bulk of orders still land in a few large, contiguous blocks.
//
// Measured on the benchmark feed: 132.2 -> 63.3 cycles/msg, a 2.09x speedup.
// See bench/README.md.
//
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "itchbook/book/order.hpp"

#ifndef ITCHBOOK_POOL_FIRST_CHUNK
#define ITCHBOOK_POOL_FIRST_CHUNK (1u << 12)   // 4096 orders, 160KB
#endif
#ifndef ITCHBOOK_POOL_MAX_CHUNK
#define ITCHBOOK_POOL_MAX_CHUNK (1u << 18)     // 262144 orders, 10.5MB
#endif

namespace itchbook::book {

class Pool {
public:
    explicit Pool(size_t first_chunk = ITCHBOOK_POOL_FIRST_CHUNK,
                  size_t max_chunk = ITCHBOOK_POOL_MAX_CHUNK)
        : chunk_size_(first_chunk == 0 ? 1 : first_chunk),
          max_chunk_(max_chunk < first_chunk ? first_chunk : max_chunk) {}

    // Pop a free order, growing by one chunk if the free list is empty.
    Order* allocate() {
        if (free_ == nullptr) {
            grow();
        }
        Order* o = free_;
        free_ = o->next;
        o->next = o->prev = nullptr;
        ++live_;
        return o;
    }

    // Push onto the free list head. The node keeps its storage; only `next` is
    // meaningful until it is handed out again.
    void deallocate(Order* o) {
        o->next = free_;
        free_ = o;
        --live_;
    }

    size_t live() const { return live_; }        // currently handed out
    size_t capacity() const { return capacity_; } // total ever allocated
    size_t chunks() const { return chunks_.size(); }

private:
    void grow() {
        // FOR OVERWRITE, NOT VALUE-INITIALISED, and this is worth 43% of the
        // cost of a grow.
        //
        // `make_unique<Order[]>` VALUE-initialises: it zeroes all 40 bytes of
        // every element. `Order` is a plain aggregate with no default member
        // initialisers, so that zeroing buys nothing -- the loop below writes
        // `next` on every element immediately, `allocate()` sets `next` and
        // `prev` on the way out, `Book::add` sets ref/shares/price/locate/side,
        // and `Side::push` sets `level_idx` unconditionally before anything
        // reads it. The only byte never written is `_pad`, and nothing compares
        // or serialises an Order bytewise -- the sole `sizeof(Order)` in the
        // tree is a static_assert.
        //
        // Measured cold, at the 10.5 MB cap: the value-initialising form costs
        // 5.85 ms and this one 3.31 ms. The memset is not additive to the page
        // faults, it IS a fault pass, and then the free-list walk below faults
        // nothing because the memset already resident-ed every page. Drop it
        // and the walk pays the faults itself, once.
        //
        // This matters because a grow lands inside the measured window. Phase
        // 10's wire-to-book histogram has its largest sample at ~5.0 ms at
        // EVERY offered rate from 1x to 25x -- 49 us of spread across a 25-fold
        // rate change -- and that sample is this function. Four max-size grows
        // happen in a 5,032,462-message replay, at messages 1,000,554 /
        // 2,013,443 / 3,030,204 / 4,047,558.
        auto chunk = std::make_unique_for_overwrite<Order[]>(chunk_size_);
        // Thread the new chunk onto the free list, back to front, so the first
        // allocations come off in address order and stay sequential in memory.
        for (size_t i = chunk_size_; i-- > 0;) {
            chunk[i].next = free_;
            free_ = &chunk[i];
        }
        capacity_ += chunk_size_;
        chunks_.push_back(std::move(chunk));
        // Double until the cap: O(log n) allocations.
        //
        // The line that used to end this sentence -- "and no single one large
        // enough for its page faults to dominate a replay" -- was wrong, and
        // phase 10 is where it showed. At the 10.5 MB cap a single grow is
        // ~2,560 pages, and its faults are the largest single latency sample in
        // the phase. Halving max_chunk halves the stall and doubles the count:
        // 1.25 MB gives 39 grows of 1.07 ms, 20 MB gives 2 of 14.57 ms. The
        // cap is therefore a tail-versus-throughput knob and not a free choice,
        // which is a thing to measure rather than assert.
        if (chunk_size_ < max_chunk_) {
            chunk_size_ = std::min(chunk_size_ * 2, max_chunk_);
        }
    }

    size_t chunk_size_;
    size_t max_chunk_;
    std::vector<std::unique_ptr<Order[]>> chunks_;
    Order* free_ = nullptr;
    size_t live_ = 0;
    size_t capacity_ = 0;
};

}  // namespace itchbook::book
