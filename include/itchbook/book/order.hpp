#pragma once
//
// order.hpp — the order node.
//
// The layout is the point. 40 bytes means an order never straddles a cache line
// badly, and the fields the hot path touches (ref, next, prev, shares) sit in
// the first 32. The static_assert below is what tells you a field reordering in
// phase 4 actually did what you thought.
//
// next/prev live *inside* the struct rather than in a wrapper node: that is why
// this is C++ and not something with a std::list. Cancel-by-reference becomes
// unlink(order) — two pointer writes, no search, no allocation.
//
#include <cstdint>

namespace itchbook::book {

// Sentinel for `level_idx`: this order rests at a price outside the dense tick
// band, so its level lives in the side's cold overflow map and is found by
// price instead of by index.
inline constexpr uint32_t kOverflowLevel = UINT32_MAX;

struct Order {
    uint64_t ref;        // 8   order reference number
    Order*   next;       // 8   intrusive FIFO — toward the back of the queue
    Order*   prev;       // 8   intrusive FIFO — toward the front
    uint32_t shares;     // 4   remaining displayed shares
    int32_t  price;      // 4   fixed point, 4 implied decimals
    uint32_t level_idx;  // 4   index into the side's dense array, or kOverflowLevel
    uint8_t  side;       // 1   'B' or 'S'
    uint8_t  _pad[3];    // 3
};

static_assert(sizeof(Order) == 40, "Order must stay one cache-line-friendly 40 bytes");
static_assert(alignof(Order) == 8, "Order must stay 8-byte aligned");

}  // namespace itchbook::book
