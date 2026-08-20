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
        auto chunk = std::make_unique<Order[]>(chunk_size_);
        // Thread the new chunk onto the free list, back to front, so the first
        // allocations come off in address order and stay sequential in memory.
        for (size_t i = chunk_size_; i-- > 0;) {
            chunk[i].next = free_;
            free_ = &chunk[i];
        }
        capacity_ += chunk_size_;
        chunks_.push_back(std::move(chunk));
        // Double until the cap: O(log n) allocations, and no single one large
        // enough for its page faults to dominate a replay.
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
