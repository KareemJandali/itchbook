#pragma once
//
// level.hpp — one price level: a FIFO queue of orders at a single price.
//
// Price-time priority means new orders join the back and executions take from
// the front. Both are O(1) here, and so is removing an order from the middle —
// which is what a cancel is, and what makes the intrusive list worth it.
//
// `shares` and `count` are maintained incrementally rather than recomputed by
// walking the list: phase 6 asks "how many shares are ahead of me at this
// price?" millions of times, and walking would make that quadratic.
//
#include <cstdint>

#include "itchbook/book/order.hpp"

namespace itchbook::book {

struct Level {
    Order*   head = nullptr;   // front of the queue — first to be executed
    Order*   tail = nullptr;   // back of the queue — where new orders join
    uint64_t shares = 0;       // total displayed shares resting here
    uint32_t count = 0;        // number of resting orders

    bool empty() const { return count == 0; }

    // Join the back of the queue. Price-time priority: later arrivals wait.
    void push_back(Order* o) {
        o->next = nullptr;
        o->prev = tail;
        if (tail != nullptr) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        shares += o->shares;
        ++count;
    }

    // Remove an order from anywhere in the queue — front, back or middle.
    void unlink(Order* o) {
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        o->next = o->prev = nullptr;
        shares -= o->shares;
        --count;
    }

    // An order's size changed in place (a partial execution or cancel). The
    // order keeps its queue position — only a replace loses priority.
    void resize(uint32_t old_shares, uint32_t new_shares) {
        shares -= old_shares;
        shares += new_shares;
    }

    // Shares resting ahead of `o` in this queue. This is the phase 6 primitive:
    // when you place a passive order you are behind exactly this many shares.
    uint64_t shares_ahead_of(const Order* o) const {
        uint64_t ahead = 0;
        for (const Order* p = head; p != nullptr && p != o; p = p->next) {
            ahead += p->shares;
        }
        return ahead;
    }
};

}  // namespace itchbook::book
