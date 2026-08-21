#pragma once
//
// live_orders.hpp — how many orders are resting at once, across a whole feed.
//
// Deliberately NOT book::RefMap. That maps a reference to an Order*, so reusing
// it would mean allocating a 40-byte Order for every order live in the day
// purely to count them. This stores the twelve bytes the count actually needs.
//
// The deletion algorithm is the same backward shift, and for the same reason:
// nearly every order in a trading day is eventually deleted or executed away,
// so tombstones would accumulate until the table degraded into a linear scan.
//
// It lives in a header rather than inside itch_census.cpp because it got a
// number wrong on its first real run, and a structure that can be wrong is a
// structure that needs a test. See accounts().
//
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itchbook::itch {

class LiveOrders {
public:
    explicit LiveOrders(size_t capacity = 1u << 20) {
        size_t cap = 16;
        while (cap < capacity) cap <<= 1;
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }

    void insert(uint64_t ref, uint32_t shares) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        if (place(ref, shares)) {
            ++inserts_;
        } else {
            ++duplicates_;   // last writer wins, as RefMap does
        }
    }

    // Mirrors Book::reduce: comparing before subtracting, because an execution
    // larger than the resting size would wrap an unsigned count to something
    // enormous. Returns true if this emptied the order.
    bool reduce(uint64_t ref, uint32_t by) {
        size_t i = find(ref);
        if (i == npos) { ++unknown_; return false; }
        if (by >= slots_[i].shares) { erase_at(i); ++emptied_; return true; }
        slots_[i].shares -= by;
        return false;
    }

    bool erase(uint64_t ref) {
        size_t i = find(ref);
        if (i == npos) { ++unknown_; return false; }
        erase_at(i);
        ++removed_;
        return true;
    }

    size_t size() const { return size_; }
    size_t peak() const { return peak_; }
    size_t capacity() const { return slots_.size(); }
    uint64_t unknown() const { return unknown_; }
    uint64_t inserts() const { return inserts_; }
    uint64_t duplicates() const { return duplicates_; }
    uint64_t removed() const { return removed_; }
    uint64_t emptied() const { return emptied_; }
    uint64_t rehashes() const { return rehashes_; }

    // Every slot that exists was inserted, and left either by a removal or by
    // being emptied. Nothing else touches the count, so this is an identity and
    // not an estimate: if it does not hold, this structure has a bug and every
    // number it reports is worthless.
    //
    // It said so on its first run against a real file, and it was right. grow()
    // rehashed by calling insert(), which counted every slot it moved as a new
    // arrival — 1,572,864 orders that never existed, on a day whose peak was
    // 1.92M against a 1<<20 start. The peak and the live count were never
    // affected (grow resets size_ and walks it straight back up), but the
    // arrival count was, and the check fired on its own bookkeeping before
    // anything downstream could quote it. Rehashing now goes through place(),
    // which moves slots without pretending they are new.
    bool accounts() const { return inserts_ == size_ + removed_ + emptied_; }

private:
    struct Slot {
        uint64_t ref = 0;
        uint32_t shares = 0;
        bool used = false;
    };
    static constexpr size_t npos = static_cast<size_t>(-1);

    // Put a reference in a slot. Returns true if it took a NEW one, which is
    // the only thing the caller can use to tell an arrival from an overwrite —
    // and the only thing grow() must not be counted as.
    bool place(uint64_t ref, uint32_t shares) {
        size_t i = ref & mask_;
        while (slots_[i].used) {
            if (slots_[i].ref == ref) {
                slots_[i].shares = shares;
                return false;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{ref, shares, true};
        ++size_;
        if (size_ > peak_) peak_ = size_;
        return true;
    }

    size_t find(uint64_t ref) const {
        size_t i = ref & mask_;
        while (slots_[i].used) {
            if (slots_[i].ref == ref) return i;
            i = (i + 1) & mask_;
        }
        return npos;
    }

    void erase_at(size_t i) {
        slots_[i].used = false;
        --size_;
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (!slots_[j].used) break;
            size_t k = slots_[j].ref & mask_;
            bool movable = (j > i) ? (k <= i || k > j) : (k <= i && k > j);
            if (movable) {
                slots_[i] = slots_[j];
                slots_[j].used = false;
                i = j;
            }
        }
    }

    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        ++rehashes_;
        for (const Slot& s : old) {
            if (s.used) place(s.ref, s.shares);
        }
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
    size_t peak_ = 0;
    uint64_t unknown_ = 0;
    uint64_t inserts_ = 0;
    uint64_t duplicates_ = 0;
    uint64_t removed_ = 0;
    uint64_t emptied_ = 0;
    uint64_t rehashes_ = 0;
};

}  // namespace itchbook::itch
