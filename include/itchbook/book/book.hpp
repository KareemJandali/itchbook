#pragma once
//
// book.hpp — the order book.
//
// Three structures, and the choice for each is the reason this is fast:
//
//   A. ref -> Order*   open-addressing hash with linear probing over a flat
//                      array. Every E/C/X/D/U message carries only an order
//                      reference, so this lookup is the hottest path in the
//                      program. A node-based unordered_map would make it one
//                      cache miss per message.
//
//   B. price level     intrusive FIFO (see level.hpp). Cancel is unlink(),
//                      two pointer writes, no search.
//
//   C. price -> level  dense array indexed by tick offset. For one symbol on
//                      one day, prices live in a narrow band; the few levels
//                      near the touch get hammered millions of times and stay
//                      resident in L1. A std::map would pointer-chase a
//                      red-black tree for every one of those hits.
//
// The dense band cannot cover every price — halts, stub quotes and sub-penny
// prints land outside it — so each side keeps a cold std::map overflow. It is
// consulted only for prices off the band or off the tick grid, which on a real
// day is a rounding error of the message count.
//
// This class knows nothing about ITCH. It takes mutations, not messages; the
// mapping from wire messages to these calls lives in dispatch.hpp.
//
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "itchbook/book/level.hpp"
#include "itchbook/book/order.hpp"
#include "itchbook/book/pool.hpp"

namespace itchbook::book {

// ---- ref -> Order* ----------------------------------------------------------

// Open addressing with linear probing. ITCH order references within a day are
// roughly monotonic, so the low bits distribute well and `ref & mask` is a
// perfectly good hash.
//
// Deletion uses Knuth's backward-shift (Algorithm R) rather than tombstones.
// That matters here more than in a general-purpose table: nearly every order in
// a trading day is eventually deleted or executed away, so tombstones would
// accumulate until the table degraded into a linear scan.
class RefMap {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    explicit RefMap(size_t capacity = 1u << 22) {
        size_t cap = 16;
        while (cap < capacity) cap <<= 1;
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }

    Order* find(uint64_t ref) const {
        size_t i = find_index(ref);
        return i == npos ? nullptr : slots_[i].ptr;
    }

    // The probe, exposed. Callers that look an order up and then delete it —
    // which is every execute, cancel, delete and replace — would otherwise walk
    // the same chain twice: once to find, once to erase. Deletes are ~45% of a
    // real feed, so that second walk is not a rounding error.
    size_t find_index(uint64_t ref) const {
        size_t i = ref & mask_;
        while (slots_[i].ptr != nullptr) {
            if (slots_[i].ref == ref) return i;
            i = (i + 1) & mask_;
        }
        return npos;
    }

    Order* at(size_t i) const { return slots_[i].ptr; }

    void insert(uint64_t ref, Order* o) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        size_t i = ref & mask_;
        while (slots_[i].ptr != nullptr) {
            if (slots_[i].ref == ref) {   // duplicate ref: last writer wins
                slots_[i].ptr = o;
                return;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{ref, o};
        ++size_;
    }

    bool erase(uint64_t ref) {
        size_t i = find_index(ref);
        if (i == npos) return false;
        erase_at(i);
        return true;
    }

    void erase_at(size_t i) {
        // Knuth 6.4 Algorithm R. Walk forward from the hole; an entry whose
        // ideal slot is not cyclically inside (hole, j] would become
        // unreachable if left where it is, so shift it back into the hole.
        slots_[i].ptr = nullptr;
        --size_;
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].ptr == nullptr) break;
            size_t k = slots_[j].ref & mask_;
            bool movable = (j > i) ? (k <= i || k > j) : (k <= i && k > j);
            if (movable) {
                slots_[i] = slots_[j];
                slots_[j].ptr = nullptr;
                i = j;
            }
        }
    }

    size_t size() const { return size_; }
    size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        uint64_t ref = 0;
        Order* ptr = nullptr;   // nullptr means the slot is empty
    };

    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.ptr != nullptr) insert(s.ref, s.ptr);
        }
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
};

// ---- one side of the book ---------------------------------------------------

struct LevelView {
    int32_t price;
    uint64_t shares;
    uint32_t count;
};

class Side {
public:
    Side(char side, int32_t tick) : side_(side), tick_(tick) {}

    char side() const { return side_; }
    bool banded() const { return !dense_.empty(); }

    // Centre the dense band on `center`, covering +/- `frac_pct` percent of it.
    // Called once, from the first order seen — the opening price is not known
    // before that. A wildly unrepresentative first price is not a correctness
    // problem: everything simply falls through to the overflow map.
    void set_band(int32_t center, int32_t frac_pct, size_t max_levels) {
        if (center <= 0 || tick_ <= 0 || banded()) return;
        int64_t half_span = static_cast<int64_t>(center) * frac_pct / 100;
        int64_t half_ticks = half_span / tick_;
        if (half_ticks < 1) half_ticks = 1;
        size_t levels = static_cast<size_t>(2 * half_ticks + 1);
        if (levels > max_levels) {
            levels = max_levels;
            half_ticks = static_cast<int64_t>(levels / 2);
        }
        base_ = static_cast<int32_t>(center - half_ticks * tick_);
        dense_.assign(levels, Level{});
        reset_cursor();
    }

    // Dense index for a price, or kOverflowLevel if it is off the band or off
    // the tick grid. Off-grid prices must go to overflow: two distinct prices
    // sharing an index would silently merge two levels into one.
    uint32_t index_of(int32_t price) const {
        if (!banded() || price < base_) return kOverflowLevel;
        int64_t diff = static_cast<int64_t>(price) - base_;
        if (diff % tick_ != 0) return kOverflowLevel;
        int64_t idx = diff / tick_;
        if (idx >= static_cast<int64_t>(dense_.size())) return kOverflowLevel;
        return static_cast<uint32_t>(idx);
    }

    void push(Order* o) {
        uint32_t idx = index_of(o->price);
        o->level_idx = idx;
        if (idx == kOverflowLevel) {
            overflow_[o->price].push_back(o);
        } else {
            Level& lvl = dense_[idx];
            bool was_empty = lvl.empty();
            lvl.push_back(o);
            if (was_empty) level_filled(idx);
        }
    }

    void pop(Order* o) {
        if (o->level_idx == kOverflowLevel) {
            auto it = overflow_.find(o->price);
            if (it == overflow_.end()) return;
            it->second.unlink(o);
            if (it->second.empty()) overflow_.erase(it);   // keep the map cold
        } else {
            Level& lvl = dense_[o->level_idx];
            lvl.unlink(o);
            if (lvl.empty()) level_emptied(o->level_idx);
        }
    }

    Level* level_at(int32_t price) {
        uint32_t idx = index_of(price);
        if (idx != kOverflowLevel) {
            return dense_[idx].empty() ? nullptr : &dense_[idx];
        }
        auto it = overflow_.find(price);
        return it == overflow_.end() ? nullptr : &it->second;
    }

    Level* level_of(const Order* o) {
        if (o->level_idx == kOverflowLevel) {
            auto it = overflow_.find(o->price);
            return it == overflow_.end() ? nullptr : &it->second;
        }
        return &dense_[o->level_idx];
    }

    bool has_levels() const { return dense_nonempty_ > 0 || !overflow_.empty(); }

    // Best price on this side: highest bid, lowest ask. Never scanned for from
    // scratch — the dense cursor tracks it and the (usually empty) overflow map
    // is checked at its own best end.
    bool best(int32_t* out) const {
        bool have = false;
        int32_t best_price = 0;
        if (dense_nonempty_ > 0 && cursor_valid()) {
            best_price = price_at(static_cast<size_t>(cursor_));
            have = true;
        }
        if (!overflow_.empty()) {
            int32_t o = is_bid() ? overflow_.rbegin()->first : overflow_.begin()->first;
            if (!have || better(o, best_price)) {
                best_price = o;
                have = true;
            }
        }
        if (have && out != nullptr) *out = best_price;
        return have;
    }

    // The `levels` best non-empty levels, best first — a merge of the dense
    // walk and the overflow map, both already in price order.
    void top(size_t levels, std::vector<LevelView>* out) const {
        out->clear();
        int64_t di = cursor_;
        auto oit_fwd = overflow_.begin();
        auto oit_rev = overflow_.rbegin();

        auto dense_has = [&]() {
            return dense_nonempty_ > 0 && di >= 0 && di < static_cast<int64_t>(dense_.size());
        };
        auto overflow_has = [&]() {
            return is_bid() ? oit_rev != overflow_.rend() : oit_fwd != overflow_.end();
        };
        auto advance_dense = [&]() { di += is_bid() ? -1 : 1; };

        // Park the dense walker on a non-empty level before each comparison.
        auto settle_dense = [&]() {
            while (dense_has() && dense_[static_cast<size_t>(di)].empty()) advance_dense();
        };

        settle_dense();
        while (out->size() < levels && (dense_has() || overflow_has())) {
            bool take_dense;
            if (!overflow_has()) {
                take_dense = true;
            } else if (!dense_has()) {
                take_dense = false;
            } else {
                int32_t dp = price_at(static_cast<size_t>(di));
                int32_t op = is_bid() ? oit_rev->first : oit_fwd->first;
                take_dense = better(dp, op);
            }

            if (take_dense) {
                const Level& lvl = dense_[static_cast<size_t>(di)];
                out->push_back({price_at(static_cast<size_t>(di)), lvl.shares, lvl.count});
                advance_dense();
                settle_dense();
            } else if (is_bid()) {
                out->push_back({oit_rev->first, oit_rev->second.shares, oit_rev->second.count});
                ++oit_rev;
            } else {
                out->push_back({oit_fwd->first, oit_fwd->second.shares, oit_fwd->second.count});
                ++oit_fwd;
            }
        }
    }

    size_t level_count() const { return dense_nonempty_ + overflow_.size(); }
    size_t overflow_count() const { return overflow_.size(); }

private:
    bool is_bid() const { return side_ == 'B'; }
    bool better(int32_t a, int32_t b) const { return is_bid() ? a > b : a < b; }
    int32_t price_at(size_t idx) const {
        return static_cast<int32_t>(base_ + static_cast<int64_t>(idx) * tick_);
    }
    bool cursor_valid() const {
        return cursor_ >= 0 && cursor_ < static_cast<int64_t>(dense_.size());
    }
    void reset_cursor() { cursor_ = is_bid() ? -1 : static_cast<int64_t>(dense_.size()); }

    void level_filled(uint32_t idx) {
        ++dense_nonempty_;
        auto i = static_cast<int64_t>(idx);
        if (dense_nonempty_ == 1 || !cursor_valid() || better(price_at(idx), price_at(static_cast<size_t>(cursor_)))) {
            cursor_ = i;
        }
    }

    // The touch moves by one tick almost always, so walking outward from the
    // old cursor is amortised O(1) — that is the whole reason for the cursor.
    void level_emptied(uint32_t idx) {
        --dense_nonempty_;
        if (dense_nonempty_ == 0) {
            reset_cursor();
            return;
        }
        if (static_cast<int64_t>(idx) != cursor_) return;
        int64_t step = is_bid() ? -1 : 1;
        int64_t i = cursor_ + step;
        while (i >= 0 && i < static_cast<int64_t>(dense_.size())) {
            if (!dense_[static_cast<size_t>(i)].empty()) {
                cursor_ = i;
                return;
            }
            i += step;
        }
        reset_cursor();
    }

    char side_;
    int32_t tick_;
    int32_t base_ = 0;
    std::vector<Level> dense_;
    std::map<int32_t, Level> overflow_;   // cold: off-band and off-tick prices
    int64_t cursor_ = -1;
    size_t dense_nonempty_ = 0;
};

// ---- the book ---------------------------------------------------------------

class Book {
public:
    // `tick` is the dense grid spacing in Price(4) units — 100 is a penny.
    // `band_pct` is how far either side of the first price the grid reaches.
    // `refs_capacity` is the ref map's slot count, rounded up to a power of two.
    // It is a cache-locality knob, not just a memory one: every message carries
    // an order reference, so a table too large to sit in cache costs a miss on
    // the hottest lookup in the program. See bench/README.md.
    explicit Book(int32_t tick = 100, int32_t band_pct = 20,
                  size_t refs_capacity = 1u << 20)
        : band_pct_(band_pct), refs_(refs_capacity),
          bids_('B', tick), asks_('S', tick) {}

    // ---- the seven mutating operations ----

    void add(uint64_t ref, char side, int32_t price, uint32_t shares) {
        if (!bids_.banded()) {
            bids_.set_band(price, band_pct_, kMaxDenseLevels);
            asks_.set_band(price, band_pct_, kMaxDenseLevels);
        }
        Order* o = pool_.allocate();
        o->ref = ref;
        o->shares = shares;
        o->price = price;
        o->side = static_cast<uint8_t>(side);
        side_for(side).push(o);
        refs_.insert(ref, o);
        resting_shares_ += shares;
    }

    // 'E' — trades at the resting order's own price.
    void execute(uint64_t ref, uint32_t shares) {
        size_t slot = refs_.find_index(ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return; }
        Order* o = refs_.at(slot);
        record_trade(o->price, shares);
        reduce(o, shares, slot);
    }

    // 'C' — trades at a stated price. A non-printable execution still removes
    // the shares from the book but must not count toward volume, VWAP or OHLC.
    void execute_with_price(uint64_t ref, uint32_t shares, int32_t price, bool printable) {
        size_t slot = refs_.find_index(ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return; }
        if (printable) record_trade(price, shares);
        reduce(refs_.at(slot), shares, slot);
    }

    // 'X' — partial cancel. Not a trade: no volume.
    void cancel(uint64_t ref, uint32_t shares) {
        size_t slot = refs_.find_index(ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return; }
        reduce(refs_.at(slot), shares, slot);
    }

    // 'D' — full removal.
    void remove(uint64_t ref) {
        size_t slot = refs_.find_index(ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return; }
        destroy(refs_.at(slot), slot);
    }

    // 'U' — delete then add. Side is inherited from the original order; the
    // replacement joins the back of its new level, losing queue priority.
    void replace(uint64_t orig_ref, uint64_t new_ref, int32_t price, uint32_t shares) {
        size_t slot = refs_.find_index(orig_ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return; }
        Order* o = refs_.at(slot);
        char side = static_cast<char>(o->side);
        destroy(o, slot);
        add(new_ref, side, price, shares);
    }

    // ---- executions that never touch the book ----

    void trade(int32_t price, uint64_t shares) {     // 'P'
        hidden_volume_ += shares;
        record_trade(price, shares);
    }

    void cross(int32_t price, uint64_t shares, char type) {   // 'Q'
        cross_volume_ += shares;
        cross_prices_[type] = price;
        record_trade(price, shares);
    }

    // ---- queries ----

    bool best_bid(int32_t* out) const { return bids_.best(out); }
    bool best_ask(int32_t* out) const { return asks_.best(out); }

    void top(char side, size_t levels, std::vector<LevelView>* out) const {
        (side == 'B' ? bids_ : asks_).top(levels, out);
    }

    // Best bid >= best ask. Never legal in a correct book.
    bool crossed() const {
        int32_t b = 0;
        int32_t a = 0;
        if (!bids_.best(&b) || !asks_.best(&a)) return false;
        return b >= a;
    }

    uint64_t shares_at(char side, int32_t price) {
        Level* lvl = (side == 'B' ? bids_ : asks_).level_at(price);
        return lvl == nullptr ? 0 : lvl->shares;
    }

    const Order* find(uint64_t ref) const { return refs_.find(ref); }
    size_t resting_orders() const { return pool_.live(); }
    uint64_t resting_shares() const { return resting_shares_; }

    uint64_t volume() const { return volume_; }
    uint64_t notional() const { return notional_; }
    uint64_t trades() const { return trades_; }
    uint64_t hidden_volume() const { return hidden_volume_; }
    uint64_t cross_volume() const { return cross_volume_; }
    const std::map<char, int32_t>& cross_prices() const { return cross_prices_; }
    bool has_trades() const { return trades_ > 0; }
    int32_t open() const { return open_; }
    int32_t high() const { return high_; }
    int32_t low() const { return low_; }
    int32_t close() const { return close_; }
    double vwap() const {
        return volume_ == 0 ? 0.0
                            : static_cast<double>(notional_) / static_cast<double>(volume_);
    }

    uint64_t unknown_ref() const { return unknown_ref_; }
    size_t overflow_levels() const { return bids_.overflow_count() + asks_.overflow_count(); }
    const Pool& pool() const { return pool_; }
    const RefMap& refs() const { return refs_; }

    // Session state, tracked for the summary and for phase 7.
    void set_trading_state(char c) { trading_state_ = c; }
    void set_system_event(char c) { system_event_ = c; }
    char trading_state() const { return trading_state_; }
    char system_event() const { return system_event_; }

private:
    // A dense band this wide already covers a $40,000 stock at penny ticks; the
    // cap only stops a nonsense first price from asking for a huge allocation.
    static constexpr size_t kMaxDenseLevels = 1u << 22;

    Side& side_for(char side) { return side == 'B' ? bids_ : asks_; }

    void record_trade(int32_t price, uint64_t shares) {
        volume_ += shares;
        notional_ += static_cast<uint64_t>(price) * shares;
        ++trades_;
        if (open_ < 0) open_ = price;
        close_ = price;
        if (high_ < 0 || price > high_) high_ = price;
        if (low_ < 0 || price < low_) low_ = price;
    }

    // Take `by` shares off an order, removing it if that empties it. Comparing
    // before subtracting matters: shares is unsigned, and an execution larger
    // than the resting size would wrap it to something enormous.
    void reduce(Order* o, uint32_t by, size_t slot) {
        if (by >= o->shares) {
            destroy(o, slot);
            return;
        }
        Level* lvl = side_for(static_cast<char>(o->side)).level_of(o);
        if (lvl != nullptr) lvl->resize(o->shares, o->shares - by);
        resting_shares_ -= by;
        o->shares -= by;
    }

    // `slot` is where the caller already found this order, so the ref map is
    // walked once per message rather than twice.
    void destroy(Order* o, size_t slot) {
        resting_shares_ -= o->shares;
        side_for(static_cast<char>(o->side)).pop(o);
        refs_.erase_at(slot);
        pool_.deallocate(o);
    }

    int32_t band_pct_;
    Pool pool_;
    RefMap refs_;
    Side bids_;
    Side asks_;

    uint64_t resting_shares_ = 0;
    uint64_t volume_ = 0;
    uint64_t notional_ = 0;
    uint64_t trades_ = 0;
    uint64_t hidden_volume_ = 0;
    uint64_t cross_volume_ = 0;
    uint64_t unknown_ref_ = 0;
    std::map<char, int32_t> cross_prices_;
    int32_t open_ = -1;
    int32_t high_ = -1;
    int32_t low_ = -1;
    int32_t close_ = -1;
    char trading_state_ = '\0';
    char system_event_ = '\0';
};

}  // namespace itchbook::book
