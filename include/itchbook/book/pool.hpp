#pragma once
//
// pool.hpp — slab allocator for Order nodes.
//
// Never `new` an Order on the hot path. Orders are allocated in large chunks up
// front and handed out from a free list; allocation becomes a pointer pop and
// deallocation a pointer push.
//
// Chunks are fixed-size and never reallocated, so every Order* stays valid for
// the life of the pool. That matters: the price levels hold raw pointers into
// this storage, and a std::vector<Order> that grew would dangle every one of
// them.
//
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "itchbook/book/order.hpp"

namespace itchbook::book {

class Pool {
public:
    explicit Pool(size_t chunk_size = 1u << 20) : chunk_size_(chunk_size) {}

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
    }

    size_t chunk_size_;
    std::vector<std::unique_ptr<Order[]>> chunks_;
    Order* free_ = nullptr;
    size_t live_ = 0;
    size_t capacity_ = 0;
};

}  // namespace itchbook::book
