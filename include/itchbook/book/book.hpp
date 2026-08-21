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
#include <memory>
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

// ---- the storage a book mutates ---------------------------------------------

// A single-symbol book owns one of these. Every book in a BookSet shares ONE,
// and that is not an optimisation to be measured later — it is the only way the
// thing fits. A per-book RefMap at the default 1<<20 slots is 16 MB, so nine
// thousand books would want ~144 GB before a single order existed.
//
// One shared reference map is correct rather than merely cheap: ITCH order
// references are unique across the whole day's feed, so two symbols can never
// name the same order.
//
// The pool is shared for a second reason as well. Allocation order follows
// arrival order, so orders that arrive together sit together and a cache line
// pulled in for one message is warm for the next; 8,700 independent free lists
// threaded through the same chunks would lose that. Whether it actually pays is
// measured in phase 9.9, against a prediction written first.
struct Storage {
    RefMap refs;
    Pool   pool;

    explicit Storage(size_t refs_capacity = 1u << 20) : refs(refs_capacity) {}
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
        }
        set_band_slots(center, levels);
    }

    // The same band, sized in SLOTS rather than as a fraction of the price.
    //
    // A percentage was the natural thing to write when a book meant one symbol.
    // Across 8,700 of them it stops being one: total band memory is
    // symbols x 2 x slots x sizeof(Level), and a percentage leaves the term
    // that decides whether the thing fits in the machine free to vary with the
    // price of whatever happened to list. A slot count is a budget you can
    // state before the run and check afterwards.
    //
    // Unlike set_band() this does NOT refuse to act on a side that already has
    // a band, because moving one is the whole point of a re-centre. The caller
    // owns the orders: this forgets where they were.
    void set_band_slots(int32_t center, size_t levels) {
        if (center <= 0 || tick_ <= 0 || levels == 0) return;
        const int64_t half_ticks = static_cast<int64_t>(levels / 2);
        int64_t base = static_cast<int64_t>(center) - half_ticks * tick_;
        if (base < 0) base = 0;
        // Snap to the tick grid, and not as tidiness. index_of() addresses a
        // slot as (price - base_) / tick_ and sends anything with a remainder
        // to overflow, so a base half a tick out of line makes EVERY price
        // off-grid and the entire book falls through to the std::map. It costs
        // no correctness -- overflow is still found by price -- which is what
        // makes it dangerous: the book stays right and quietly stops being
        // fast. A centre of (bid+ask)/2 across a one-tick spread is exactly
        // that case, so it arrived the moment the band started being centred
        // on a quote instead of on an order.
        base -= base % tick_;
        base_ = static_cast<int32_t>(base);
        dense_.assign(levels, Level{});
        dense_nonempty_ = 0;
        reset_cursor();
    }

    // Forget every level. The orders are not freed and not owned here; the
    // caller is holding them and is about to push them somewhere else.
    void clear_levels() {
        dense_.clear();
        overflow_.clear();
        dense_nonempty_ = 0;
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

    // The best level on this side, or nullptr when the side is empty. The
    // matching engine needs the level, not just its price: matching consumes
    // the FIFO from the front, one resting order at a time.
    Level* best_level() {
        int32_t px = 0;
        if (!best(&px)) return nullptr;
        return level_at(px);
    }

    const Level* best_level() const {
        int32_t px = 0;
        if (!best(&px)) return nullptr;
        return level_at(px);
    }

    Level* level_at(int32_t price) {
        uint32_t idx = index_of(price);
        if (idx != kOverflowLevel) {
            return dense_[idx].empty() ? nullptr : &dense_[idx];
        }
        auto it = overflow_.find(price);
        return it == overflow_.end() ? nullptr : &it->second;
    }

    const Level* level_at(int32_t price) const {
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

    // Every order resting on this side, with mutable access, in no meaningful
    // order. One caller: clearing a book whose pool and reference map belong to
    // 8,700 other books, which cannot drop the containers and walk away.
    //
    // The dense scan stops as soon as it has seen every non-empty level, so it
    // costs the occupied part of the band rather than the band. `fn` is allowed
    // to destroy the order it is handed, which is why the successor is read
    // first: deallocating an order overwrites its `next`.
    template <typename Fn>
    void for_each_order_mut(Fn&& fn) {
        size_t remaining = dense_nonempty_;
        for (size_t i = 0; i < dense_.size() && remaining > 0; ++i) {
            Level& lvl = dense_[i];
            if (lvl.empty()) continue;
            --remaining;
            for (Order* o = lvl.head; o != nullptr;) {
                Order* next = o->next;
                fn(o);
                o = next;
            }
        }
        for (auto& entry : overflow_) {
            for (Order* o = entry.second.head; o != nullptr;) {
                Order* next = o->next;
                fn(o);
                o = next;
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
    // Owning: one book with a reference map and a pool to itself. Every caller
    // that is not a BookSet, and identical in behaviour to before the storage
    // moved out from under it.
    explicit Book(int32_t tick = 100, int32_t band_pct = 20,
                  size_t refs_capacity = 1u << 20)
        : owned_(std::make_unique<Storage>(refs_capacity)), store_(owned_.get()),
          tick_(tick), band_pct_(band_pct), bids_('B', tick), asks_('S', tick) {}

    // Borrowing: the storage belongs to the BookSet and outlives every book in
    // it. `locate` is carried so a book knows which symbol it is without asking
    // the set; it is not how messages are routed — every ITCH message already
    // carries its locate in the common header — and phase 9.3 stamps it into
    // each Order, where it becomes a cross-check that a reference resolved to
    // an order belonging to the symbol the message named.
    Book(Storage& store, uint16_t locate, int32_t tick = 100, int32_t band_pct = 20)
        : store_(&store), locate_(locate),
          tick_(tick), band_pct_(band_pct), bids_('B', tick), asks_('S', tick) {}

    // Throw away every resting order and start again from empty.
    //
    // For recovery after a sequence gap. The messages we missed could have
    // added, cancelled, executed or replaced anything, so no order in the book
    // can be shown to still be correct — and a book that is wrong in an unknown
    // way is worse than an empty one, because an empty book at least knows what
    // it does not know.
    //
    // The TAPE statistics survive: volume, VWAP, OHLC and the cross prices are
    // facts about trades we actually saw printed, and a gap does not make the
    // prints before it un-happen. They are incomplete after a gap, which is a
    // different thing from wrong, and `unknown_ref` records the difference.
    void clear_orders() {
        release_all_orders();
        bids_ = Side('B', tick_);
        asks_ = Side('S', tick_);
        resting_orders_ = 0;
        resting_shares_ = 0;
    }

    // ---- the seven mutating operations ----

    void add(uint64_t ref, char side, int32_t price, uint32_t shares) {
        if (!bids_.banded()) {
            if (band_levels_ == 0) {
                // The phase-3 policy, unchanged for every caller that has not
                // asked for a slot budget: centre on the first price seen.
                bids_.set_band(price, band_pct_, kMaxDenseLevels);
                asks_.set_band(price, band_pct_, kMaxDenseLevels);
            } else {
                open_band_when_two_sided(side, price);
            }
        }
        Order* o = store_->pool.allocate();
        o->ref = ref;
        o->shares = shares;
        o->price = price;
        o->locate = locate_;
        o->side = static_cast<uint8_t>(side);
        side_for(side).push(o);
        store_->refs.insert(ref, o);
        ++resting_orders_;
        ++adds_;
        if (o->level_idx == kOverflowLevel) ++off_band_adds_;
        resting_shares_ += shares;
        maybe_recentre();
    }

    // 'E' — trades at the resting order's own price.
    void execute(uint64_t ref, uint32_t shares) {
        size_t slot = resolve(ref);
        if (slot == RefMap::npos) return;
        Order* o = store_->refs.at(slot);
        record_trade(o->price, shares);
        reduce(o, shares, slot);
    }

    // 'C' — trades at a stated price. A non-printable execution still removes
    // the shares from the book but must not count toward volume, VWAP or OHLC.
    void execute_with_price(uint64_t ref, uint32_t shares, int32_t price, bool printable) {
        size_t slot = resolve(ref);
        if (slot == RefMap::npos) return;
        if (printable) record_trade(price, shares);
        reduce(store_->refs.at(slot), shares, slot);
    }

    // 'X' — partial cancel. Not a trade: no volume.
    void cancel(uint64_t ref, uint32_t shares) {
        size_t slot = resolve(ref);
        if (slot == RefMap::npos) return;
        reduce(store_->refs.at(slot), shares, slot);
    }

    // 'D' — full removal.
    void remove(uint64_t ref) {
        size_t slot = resolve(ref);
        if (slot == RefMap::npos) return;
        destroy(store_->refs.at(slot), slot);
    }

    // 'U' — delete then add. Side is inherited from the original order; the
    // replacement joins the back of its new level, losing queue priority.
    void replace(uint64_t orig_ref, uint64_t new_ref, int32_t price, uint32_t shares) {
        size_t slot = resolve(orig_ref);
        if (slot == RefMap::npos) return;
        Order* o = store_->refs.at(slot);
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

    // A locked book (bid == ask) is legal on a real feed for brief moments;
    // a strictly crossed one (bid > ask) is not. crossed() lumps them together
    // and is left alone because the matching engine's fuzz invariant is written
    // against it — the simulator needs to tell them apart, because an incoming
    // order that *locks* our resting quote takes us out, and that is a fill.
    bool locked() const {
        int32_t b = 0;
        int32_t a = 0;
        if (!bids_.best(&b) || !asks_.best(&a)) return false;
        return b == a;
    }

    bool strictly_crossed() const {
        int32_t b = 0;
        int32_t a = 0;
        if (!bids_.best(&b) || !asks_.best(&a)) return false;
        return b > a;
    }

    // Best bid >= best ask. Never legal in a correct book.
    bool crossed() const {
        int32_t b = 0;
        int32_t a = 0;
        if (!bids_.best(&b) || !asks_.best(&a)) return false;
        return b >= a;
    }

    // Front of the queue on one side: the order that price-time priority says
    // trades next. nullptr when that side is empty.
    const Order* best_order(char side) {
        Level* lvl = (side == 'B' ? bids_ : asks_).best_level();
        return lvl == nullptr ? nullptr : lvl->head;
    }

    const Order* best_order(char side) const {
        const Level* lvl = (side == 'B' ? bids_ : asks_).best_level();
        return lvl == nullptr ? nullptr : lvl->head;
    }

    // Front of the queue at an arbitrary price, not just the best one. The
    // simulator walks this list at order-arrival time to record which order
    // references were resting ahead of it — which is the whole reason
    // ahead-versus-behind is answerable on an order-by-order feed at all.
    const Order* first_order(char side, int32_t price) const {
        const Level* lvl = (side == 'B' ? bids_ : asks_).level_at(price);
        return lvl == nullptr ? nullptr : lvl->head;
    }

    // Take shares off a resting order because they traded, as opposed to
    // cancel(), which takes them off because the owner withdrew them. The book
    // mutation is identical; the distinction is that the matching engine does
    // its own fill accounting and must not also move this book's market trade
    // statistics, which describe the *feed's* trades and not ours.
    void take(uint64_t ref, uint32_t shares) {
        size_t slot = resolve(ref);
        if (slot == RefMap::npos) return;
        reduce(store_->refs.at(slot), shares, slot);
    }

    uint64_t shares_at(char side, int32_t price) {
        Level* lvl = (side == 'B' ? bids_ : asks_).level_at(price);
        return lvl == nullptr ? 0 : lvl->shares;
    }

    uint64_t shares_at(char side, int32_t price) const {
        const Level* lvl = (side == 'B' ? bids_ : asks_).level_at(price);
        return lvl == nullptr ? 0 : lvl->shares;
    }

    // Agrees with resolve() by construction: if these two disagreed, apply_ex()
    // would record a pre-mutation state for an order the mutation then refused
    // to touch, and the simulator would be reasoning about another symbol's
    // book. Const, so it counts nothing — the mutating path does the counting.
    const Order* find(uint64_t ref) const {
        const Order* o = store_->refs.find(ref);
        return (o != nullptr && o->locate == locate_) ? o : nullptr;
    }

    // Was pool_.live(). A shared pool counts every symbol's orders, so this has
    // to be the book's own tally or an all-symbols run would report the whole
    // market's resting order count for each of 8,700 books.
    size_t resting_orders() const { return resting_orders_; }
    uint16_t locate() const { return locate_; }
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

    // ---- state capture, for the snapshot ------------------------------------
    //
    // Everything the book accumulates that is NOT derivable from replaying the
    // orders: the tape. A process restarting mid-day has to come back with the
    // day's volume and range intact, because they are facts about trades that
    // happened and no amount of re-reading the current book recovers them.
    struct TapeState {
        uint64_t volume = 0;
        uint64_t notional = 0;
        uint64_t trades = 0;
        uint64_t hidden_volume = 0;
        uint64_t cross_volume = 0;
        uint64_t unknown_ref = 0;
        int32_t open = -1;
        int32_t high = -1;
        int32_t low = -1;
        int32_t close = -1;
        char trading_state = '\0';
        char system_event = '\0';
    };

    TapeState tape() const {
        return TapeState{volume_,        notional_,      trades_,
                         hidden_volume_, cross_volume_,  unknown_ref_,
                         open_,          high_,          low_,
                         close_,         trading_state_, system_event_};
    }

    void restore_tape(const TapeState& t) {
        volume_ = t.volume;
        notional_ = t.notional;
        trades_ = t.trades;
        hidden_volume_ = t.hidden_volume;
        cross_volume_ = t.cross_volume;
        unknown_ref_ = t.unknown_ref;
        open_ = t.open;
        high_ = t.high;
        low_ = t.low;
        close_ = t.close;
        trading_state_ = t.trading_state;
        system_event_ = t.system_event;
    }

    // Every resting order, in the order the exchange would fill them: best
    // price first, and within a price, oldest first.
    //
    // The traversal order is the whole point. A snapshot that dumps the ref map
    // in hash order restores a book with the right levels, the right shares and
    // scrambled queue priority — which every level-based check passes and every
    // queue model silently gets wrong. Writing them in fill order and restoring
    // by replaying adds reproduces the FIFO exactly, because add() appends.
    template <typename Fn>
    void for_each_order(char side, Fn&& fn) const {
        std::vector<LevelView> levels;
        top(side, kMaxDenseLevels * 4, &levels);
        for (const LevelView& lv : levels) {
            for (const Order* o = first_order(side, lv.price); o != nullptr; o = o->next) {
                fn(*o);
            }
        }
    }

    uint64_t unknown_ref() const { return unknown_ref_; }

    // References that resolved to an order belonging to a different symbol.
    // Zero, on any feed that is what it claims to be. See resolve().
    uint64_t locate_mismatch() const { return locate_mismatch_; }
    size_t overflow_levels() const { return bids_.overflow_count() + asks_.overflow_count(); }

    // ---- the band, as it actually behaved -----------------------------------
    //
    // Off-band is not failure. Every symbol on a real day holds STUB QUOTES --
    // orders parked at $0.0001 or $199,999.99 to satisfy a two-sided quoting
    // obligation -- and no band anyone can afford covers those; 77.6% of the
    // symbols that quoted on 2019-12-30 posted one at or above $100,000. So the
    // question a band policy has to answer is not "did anything land outside"
    // but "what fraction did, and how close to the touch was it".
    // Ask for a slot-budgeted band instead of the phase-3 percentage one. Must
    // be set before the first add: a band that moves after orders are in it is
    // a re-centre, which has its own policy and its own counter.
    void set_band_levels(size_t levels) { band_levels_ = levels; }

    uint64_t adds() const { return adds_; }
    uint64_t off_band_adds() const { return off_band_adds_; }
    uint64_t recentres() const { return recentres_; }
    size_t band_levels() const { return band_levels_; }
    const Pool& pool() const { return store_->pool; }
    const RefMap& refs() const { return store_->refs; }
    const Storage& storage() const { return *store_; }

    // Session state, tracked for the summary and for phase 7.
    void set_trading_state(char c) { trading_state_ = c; }
    void set_system_event(char c) { system_event_ = c; }
    char trading_state() const { return trading_state_; }
    char system_event() const { return system_event_; }

private:
    // A dense band this wide already covers a $40,000 stock at penny ticks; the
    // cap only stops a nonsense first price from asking for a huge allocation.
    static constexpr size_t kMaxDenseLevels = 1u << 22;

    // How long the band gets to prove itself, and how badly it has to do before
    // it is moved. Both are policy rather than physics, and both are named here
    // rather than buried at a call site so that a sweep can move them.
    static constexpr uint64_t kRecentreWindow = 1000;   // adds
    static constexpr uint64_t kRecentrePercent = 10;    // off-band, percent

    // Open the band on the first TWO-SIDED quote rather than the first order.
    //
    // The first order of an ITCH day arrives around 04:00, and on a real feed it
    // is as likely to be a stub quote as a price anyone trades at -- a band
    // centred on $199,999.99 covers nothing. Waiting for both sides costs a
    // handful of orders sitting in the overflow map and buys a centre that is at
    // least between two real quotes.
    void open_band_when_two_sided(char side, int32_t price) {
        if (side == 'B') {
            if (pending_bid_ < 0 || price > pending_bid_) pending_bid_ = price;
        } else {
            if (pending_ask_ < 0 || price < pending_ask_) pending_ask_ = price;
        }
        if (pending_bid_ < 0 || pending_ask_ < 0) return;
        move_band_to(static_cast<int32_t>(
            (static_cast<int64_t>(pending_bid_) + pending_ask_) / 2));
    }

    // One move per session, decided once, at a fixed point, on evidence.
    // Re-centring rebuilds both dense arrays and re-indexes every resting
    // order, so it is neither free nor silent: it is counted and reported.
    void maybe_recentre() {
        if (band_levels_ == 0 || recentre_checked_ || !bids_.banded()) return;
        if (adds_ < kRecentreWindow) return;
        recentre_checked_ = true;
        if (off_band_adds_ * 100 < adds_ * kRecentrePercent) return;
        int32_t bid = 0;
        int32_t ask = 0;
        if (!bids_.best(&bid) || !asks_.best(&ask)) return;
        move_band_to(static_cast<int32_t>((static_cast<int64_t>(bid) + ask) / 2));
        ++recentres_;
    }

    // Put the band somewhere else and every resting order back into it.
    //
    // Collect first: set_band_slots() throws the levels away, and the orders
    // are reached THROUGH those levels. Order matters too -- within a level the
    // walk runs head to tail and push() appends, so price-time priority
    // survives the move. Rebuilding from anything that reorders would not.
    void move_band_to(int32_t centre) {
        std::vector<Order*> all;
        all.reserve(resting_orders_);
        bids_.for_each_order_mut([&](Order* o) { all.push_back(o); });
        asks_.for_each_order_mut([&](Order* o) { all.push_back(o); });
        bids_.clear_levels();
        asks_.clear_levels();
        bids_.set_band_slots(centre, band_levels_);
        asks_.set_band_slots(centre, band_levels_);
        for (Order* o : all) side_for(static_cast<char>(o->side)).push(o);
    }

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
        --resting_orders_;
        side_for(static_cast<char>(o->side)).pop(o);
        store_->refs.erase_at(slot);
        store_->pool.deallocate(o);
    }

    // Every reference-carrying message goes through here. Two ways a reference
    // can fail to name an order this book may touch, and they are different
    // facts that get different counters:
    //
    //   * No order has it. Ordinary and expected — it is a message about the
    //     world before a gap, or before this process started. unknown_ref_.
    //
    //   * An order has it, and it belongs to another symbol. That is not
    //     expected by anything: ITCH references are unique across the day's
    //     whole feed, so with one shared map this cannot happen unless the map,
    //     the feed, or this code is wrong. locate_mismatch_ must be zero on a
    //     real day, and the mutation is refused rather than applied, because
    //     applying it would corrupt two books instead of one.
    size_t resolve(uint64_t ref) {
        size_t slot = store_->refs.find_index(ref);
        if (slot == RefMap::npos) { ++unknown_ref_; return RefMap::npos; }
        if (store_->refs.at(slot)->locate != locate_) {
            ++locate_mismatch_;
            return RefMap::npos;
        }
        return slot;
    }

    // Hand this book's orders back one at a time, because the containers belong
    // to everyone. The old implementation dropped the whole Pool and the whole
    // RefMap and rebuilt them — correct while a book was the only thing in
    // them, and a silent emptying of 8,699 other books the moment it was not. A
    // gap on one symbol is not an outage for the rest of the market.
    void release_all_orders() {
        auto release = [this](Order* o) {
            store_->refs.erase(o->ref);
            store_->pool.deallocate(o);
        };
        bids_.for_each_order_mut(release);
        asks_.for_each_order_mut(release);
    }

    // Non-null always. It points into owned_ when this book has its own
    // storage, and at the BookSet's when it does not.
    std::unique_ptr<Storage> owned_;
    Storage* store_;
    uint16_t locate_ = 0;
    int32_t tick_;
    int32_t band_pct_;
    size_t band_levels_ = 0;        // 0 = the phase-3 percentage policy
    int32_t pending_bid_ = -1;
    int32_t pending_ask_ = -1;
    bool recentre_checked_ = false;
    Side bids_;
    Side asks_;

    size_t resting_orders_ = 0;
    uint64_t adds_ = 0;
    uint64_t off_band_adds_ = 0;
    uint64_t recentres_ = 0;
    uint64_t resting_shares_ = 0;
    uint64_t volume_ = 0;
    uint64_t notional_ = 0;
    uint64_t trades_ = 0;
    uint64_t hidden_volume_ = 0;
    uint64_t cross_volume_ = 0;
    uint64_t unknown_ref_ = 0;
    uint64_t locate_mismatch_ = 0;
    std::map<char, int32_t> cross_prices_;
    int32_t open_ = -1;
    int32_t high_ = -1;
    int32_t low_ = -1;
    int32_t close_ = -1;
    char trading_state_ = '\0';
    char system_event_ = '\0';
};

}  // namespace itchbook::book
