// test_book — unit tests for the order book: pool, level, ref map, and the
// mutation semantics the differential test can only check in aggregate.
//
// The differential test proves the C++ book agrees with the oracle. These prove
// the individual structures behave as designed, so that when the two disagree
// there is somewhere to start looking.
#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "tests/check.hpp"

using namespace itchbook::book;

namespace {

constexpr int32_t kMid = 100'0000;   // $10.0000 in Price(4)
constexpr int32_t kTick = 100;       // a penny

void test_pool() {
    Pool pool(4);   // a tiny chunk, so growth happens immediately
    std::vector<Order*> handed;
    for (int i = 0; i < 10; ++i) handed.push_back(pool.allocate());
    CHECK_EQ(pool.live(), 10u);
    CHECK(pool.chunks() >= 3);

    // Every pointer must still be distinct and usable after the chunk growth
    // that happened underneath them — this is why chunks are never reallocated.
    std::sort(handed.begin(), handed.end());
    CHECK(std::adjacent_find(handed.begin(), handed.end()) == handed.end());
    for (size_t i = 0; i < handed.size(); ++i) handed[i]->ref = i;
    for (size_t i = 0; i < handed.size(); ++i) CHECK_EQ(handed[i]->ref, i);

    size_t cap = pool.capacity();
    for (Order* o : handed) pool.deallocate(o);
    CHECK_EQ(pool.live(), 0u);
    // Freed nodes are reused rather than growing the pool again.
    for (int i = 0; i < 10; ++i) pool.allocate();
    CHECK_EQ(pool.capacity(), cap);
}

void test_level_fifo() {
    Pool pool(8);
    Level lvl;
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    Order* c = pool.allocate();
    a->shares = 100;
    b->shares = 200;
    c->shares = 300;
    lvl.push_back(a);
    lvl.push_back(b);
    lvl.push_back(c);

    CHECK_EQ(lvl.count, 3u);
    CHECK_EQ(lvl.shares, 600u);
    CHECK(lvl.head == a);
    CHECK(lvl.tail == c);

    // Shares ahead of you in the queue — the phase 6 primitive.
    CHECK_EQ(lvl.shares_ahead_of(a), 0u);
    CHECK_EQ(lvl.shares_ahead_of(b), 100u);
    CHECK_EQ(lvl.shares_ahead_of(c), 300u);

    lvl.unlink(b);            // from the middle
    CHECK_EQ(lvl.count, 2u);
    CHECK_EQ(lvl.shares, 400u);
    CHECK(lvl.head->next == c);
    CHECK(c->prev == a);

    lvl.unlink(a);            // from the front
    CHECK(lvl.head == c);
    CHECK(c->prev == nullptr);

    lvl.unlink(c);            // the last one
    CHECK(lvl.empty());
    CHECK(lvl.head == nullptr);
    CHECK(lvl.tail == nullptr);
    CHECK_EQ(lvl.shares, 0u);
}

void test_refmap_basics() {
    Pool pool(64);
    RefMap map(16);
    Order* a = pool.allocate();
    Order* b = pool.allocate();

    CHECK(map.find(1) == nullptr);
    map.insert(1, a);
    map.insert(2, b);
    CHECK(map.find(1) == a);
    CHECK(map.find(2) == b);
    CHECK_EQ(map.size(), 2u);

    CHECK(map.erase(1));
    CHECK(map.find(1) == nullptr);
    CHECK(map.find(2) == b);      // the survivor stays reachable
    CHECK(!map.erase(1));         // erasing twice is not an error
    CHECK_EQ(map.size(), 1u);
}

void test_refmap_collision_chain() {
    // Refs that all hash to the same slot, so every one of them lands in the
    // same probe chain. Deleting from the middle of that chain is exactly what
    // backward-shift deletion has to get right.
    Pool pool(64);
    RefMap map(16);
    const size_t cap = map.capacity();
    std::vector<Order*> orders;
    std::vector<uint64_t> refs;
    for (uint64_t i = 0; i < 6; ++i) {
        refs.push_back(3 + i * cap);      // all hash to slot 3
        orders.push_back(pool.allocate());
        map.insert(refs.back(), orders.back());
    }
    for (size_t i = 0; i < refs.size(); ++i) CHECK(map.find(refs[i]) == orders[i]);

    map.erase(refs[2]);   // a hole in the middle of the chain
    CHECK(map.find(refs[2]) == nullptr);
    for (size_t i = 0; i < refs.size(); ++i) {
        if (i != 2) CHECK(map.find(refs[i]) == orders[i]);
    }

    map.erase(refs[0]);   // the head of the chain
    for (size_t i = 3; i < refs.size(); ++i) CHECK(map.find(refs[i]) == orders[i]);
}

void test_refmap_churn_against_reference() {
    // The real test: a million-ish operations of insert/erase churn, checked
    // against std::unordered_map. A tombstone implementation would still pass
    // the small cases above and rot here.
    Pool pool(1 << 12);
    RefMap map(16);          // starts tiny, so it rehashes many times
    std::unordered_map<uint64_t, Order*> reference;
    std::mt19937_64 rng(12345);
    std::vector<uint64_t> live;

    for (int step = 0; step < 200000; ++step) {
        bool insert = live.empty() || (rng() % 100) < 55;
        if (insert) {
            uint64_t ref = rng() % 50000;
            if (reference.count(ref)) continue;
            Order* o = pool.allocate();
            o->ref = ref;
            map.insert(ref, o);
            reference[ref] = o;
            live.push_back(ref);
        } else {
            size_t i = rng() % live.size();
            uint64_t ref = live[i];
            live[i] = live.back();
            live.pop_back();
            CHECK(map.erase(ref));
            pool.deallocate(reference[ref]);
            reference.erase(ref);
        }
    }

    CHECK_EQ(map.size(), reference.size());
    for (const auto& [ref, ptr] : reference) CHECK(map.find(ref) == ptr);
    // Everything erased must stay unreachable, not resurface from a broken shift.
    int ghosts = 0;
    for (uint64_t ref = 0; ref < 50000; ++ref) {
        if (!reference.count(ref) && map.find(ref) != nullptr) ++ghosts;
    }
    CHECK_EQ(ghosts, 0);
}

void test_best_price_cursor() {
    Book book(kTick);
    book.add(1, 'B', kMid - kTick, 100);
    book.add(2, 'B', kMid - 2 * kTick, 100);
    book.add(3, 'S', kMid + kTick, 100);
    book.add(4, 'S', kMid + 2 * kTick, 100);

    int32_t bid = 0;
    int32_t ask = 0;
    CHECK(book.best_bid(&bid));
    CHECK(book.best_ask(&ask));
    CHECK_EQ(bid, kMid - kTick);
    CHECK_EQ(ask, kMid + kTick);
    CHECK(!book.crossed());

    // Empty the touch: the cursor must walk out one level, not rescan.
    book.remove(1);
    CHECK(book.best_bid(&bid));
    CHECK_EQ(bid, kMid - 2 * kTick);
    book.remove(3);
    CHECK(book.best_ask(&ask));
    CHECK_EQ(ask, kMid + 2 * kTick);

    // ...and back in when a better price arrives.
    book.add(5, 'B', kMid, 100);
    CHECK(book.best_bid(&bid));
    CHECK_EQ(bid, kMid);

    book.remove(2);
    book.remove(5);
    CHECK(!book.best_bid(&bid));    // an empty side has no best price
    CHECK(!book.crossed());
}

void test_overflow_band() {
    Book book(kTick);
    book.add(1, 'B', kMid, 100);            // centres the band here

    // Far outside +/-20%, and off the tick grid: both must land in the cold
    // overflow map and still be found, priced and ordered correctly.
    book.add(2, 'B', kMid + 900000, 200);   // way above the band
    book.add(3, 'B', kMid + 37, 300);       // off-grid by 37 hundredths of a cent
    CHECK_EQ(book.overflow_levels(), 2u);

    int32_t bid = 0;
    CHECK(book.best_bid(&bid));
    CHECK_EQ(bid, kMid + 900000);           // the overflow price is the best bid

    std::vector<LevelView> top;
    book.top('B', 10, &top);
    CHECK_EQ(top.size(), 3u);
    CHECK_EQ(top[0].price, kMid + 900000);  // descending, merged across both stores
    CHECK_EQ(top[1].price, kMid + 37);
    CHECK_EQ(top[2].price, kMid);
    CHECK_EQ(top[0].shares, 200u);
    CHECK_EQ(top[1].shares, 300u);

    // Two distinct off-grid prices must not collapse into one dense slot.
    book.add(4, 'B', kMid + 38, 400);
    CHECK_EQ(book.shares_at('B', kMid + 37), 300u);
    CHECK_EQ(book.shares_at('B', kMid + 38), 400u);

    book.remove(2);
    CHECK(book.best_bid(&bid));
    CHECK_EQ(bid, kMid + 38);
    book.remove(3);
    book.remove(4);
    CHECK_EQ(book.overflow_levels(), 0u);   // emptied overflow levels are dropped
    CHECK(book.best_bid(&bid));
    CHECK_EQ(bid, kMid);
}

void test_top_ordering_and_depth() {
    Book book(kTick);
    for (int i = 0; i < 5; ++i) {
        book.add(static_cast<uint64_t>(100 + i), 'B', kMid - (i + 1) * kTick, 10 + i);
        book.add(static_cast<uint64_t>(200 + i), 'S', kMid + (i + 1) * kTick, 20 + i);
    }
    std::vector<LevelView> top;

    book.top('B', 3, &top);
    CHECK_EQ(top.size(), 3u);
    CHECK_EQ(top[0].price, kMid - kTick);         // bids descend
    CHECK_EQ(top[2].price, kMid - 3 * kTick);

    book.top('S', 3, &top);
    CHECK_EQ(top[0].price, kMid + kTick);         // asks ascend
    CHECK_EQ(top[2].price, kMid + 3 * kTick);

    book.top('B', 50, &top);                      // asking past the depth
    CHECK_EQ(top.size(), 5u);

    // Two orders at one price aggregate into a single level.
    book.add(999, 'B', kMid - kTick, 5);
    book.top('B', 1, &top);
    CHECK_EQ(top[0].shares, 15u);
    CHECK_EQ(top[0].count, 2u);
}

void test_mutation_semantics() {
    Book book(kTick);
    book.add(1, 'B', kMid, 100);

    // 'E' trades at the resting order's own price.
    book.execute(1, 40);
    CHECK_EQ(book.shares_at('B', kMid), 60u);
    CHECK_EQ(book.volume(), 40u);
    CHECK_EQ(book.notional(), static_cast<uint64_t>(kMid) * 40);
    CHECK_EQ(book.resting_shares(), 60u);

    // 'C' trades at a stated price...
    book.execute_with_price(1, 10, kMid - 50, true);
    CHECK_EQ(book.volume(), 50u);
    CHECK_EQ(book.notional(), static_cast<uint64_t>(kMid) * 40 + static_cast<uint64_t>(kMid - 50) * 10);
    CHECK_EQ(book.shares_at('B', kMid), 50u);   // ...and the order stays at its own

    // ...unless it is non-printable, which moves the book but not the volume.
    book.execute_with_price(1, 10, kMid, false);
    CHECK_EQ(book.volume(), 50u);
    CHECK_EQ(book.shares_at('B', kMid), 40u);

    // An execution larger than what is resting removes the order. Comparing
    // before subtracting is what stops the unsigned share count wrapping.
    book.execute(1, 1000);
    CHECK(book.find(1) == nullptr);
    CHECK_EQ(book.resting_orders(), 0u);
    CHECK_EQ(book.resting_shares(), 0u);
    CHECK_EQ(book.volume(), 1050u);

    // A cancel is not a trade.
    book.add(2, 'S', kMid + kTick, 100);
    uint64_t vol = book.volume();
    book.cancel(2, 30);
    CHECK_EQ(book.shares_at('S', kMid + kTick), 70u);
    CHECK_EQ(book.volume(), vol);
    book.cancel(2, 70);
    CHECK(book.find(2) == nullptr);
}

void test_replace_loses_priority_and_inherits_side() {
    Book book(kTick);
    book.add(1, 'S', kMid, 100);
    book.add(2, 'S', kMid, 100);
    book.replace(1, 9, kMid, 100);

    // Ref 1 was at the front; its replacement is now behind ref 2.
    Level* lvl = nullptr;
    std::vector<LevelView> top;
    book.top('S', 1, &top);
    CHECK_EQ(top[0].count, 2u);
    CHECK_EQ(top[0].shares, 200u);
    (void)lvl;

    const Order* replaced = book.find(9);
    CHECK(replaced != nullptr);
    CHECK_EQ(replaced->side, static_cast<uint8_t>('S'));   // side is inherited
    CHECK(book.find(1) == nullptr);
    CHECK(book.find(2)->next == replaced);                 // ...at the back

    // A replace onto a new price moves the order and cleans up the old level.
    book.replace(9, 10, kMid + kTick, 50);
    CHECK_EQ(book.shares_at('S', kMid), 100u);
    CHECK_EQ(book.shares_at('S', kMid + kTick), 50u);
}

void test_unknown_refs_and_untouched_book() {
    Book book(kTick);
    book.add(1, 'B', kMid, 100);
    book.execute(999, 10);
    book.cancel(999, 10);
    book.remove(999);
    book.replace(999, 1000, kMid, 10);
    CHECK_EQ(book.unknown_ref(), 4u);
    CHECK_EQ(book.resting_shares(), 100u);
    CHECK_EQ(book.volume(), 0u);       // a ghost execution is not volume

    // 'P' and 'Q' are real volume that never appears in the displayed book.
    book.trade(kMid, 250);
    book.cross(kMid + 200, 5000, 'O');
    CHECK_EQ(book.resting_shares(), 100u);
    CHECK_EQ(book.volume(), 5250u);
    CHECK_EQ(book.hidden_volume(), 250u);
    CHECK_EQ(book.cross_volume(), 5000u);
    CHECK_EQ(book.cross_prices().at('O'), kMid + 200);
    CHECK_EQ(book.open(), kMid);
    CHECK_EQ(book.close(), kMid + 200);
    CHECK_EQ(book.high(), kMid + 200);
    CHECK_EQ(book.low(), kMid);
}

void test_dispatch_matches_the_wire() {
    // One message of each mutating type, applied through dispatch.hpp, to prove
    // the offsets line up with what the book expects.
    Book book(kTick);
    std::vector<uint8_t> buf;
    auto msg = [&](const std::vector<uint8_t>& bytes) {
        buf = bytes;
        return buf.data();
    };
    auto be16 = [](uint16_t v) { return std::vector<uint8_t>{static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xff)}; };
    auto be32 = [](uint32_t v) {
        return std::vector<uint8_t>{static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                    static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xff)};
    };
    auto be64 = [](uint64_t v) {
        std::vector<uint8_t> out(8);
        for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (56 - 8 * i));
        return out;
    };
    auto header = [&](char type) {
        std::vector<uint8_t> h{static_cast<uint8_t>(type)};
        for (uint8_t b : be16(1)) h.push_back(b);      // stock locate
        for (uint8_t b : be16(0)) h.push_back(b);      // tracking
        for (int i = 0; i < 6; ++i) h.push_back(0);    // timestamp
        return h;
    };
    auto append = [](std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
    };

    // 'A': ref @11, side @19, shares @20, stock @24, price @32
    std::vector<uint8_t> add = header('A');
    append(add, be64(1));
    add.push_back('B');
    append(add, be32(100));
    for (int i = 0; i < 8; ++i) add.push_back(' ');
    append(add, be32(static_cast<uint32_t>(kMid)));
    CHECK_EQ(add.size(), 36u);
    CHECK(apply(book, 'A', msg(add)));
    CHECK_EQ(book.shares_at('B', kMid), 100u);

    // 'E': ref @11, shares @19, match @23
    std::vector<uint8_t> exec = header('E');
    append(exec, be64(1));
    append(exec, be32(40));
    append(exec, be64(7));
    CHECK_EQ(exec.size(), 31u);
    CHECK(apply(book, 'E', msg(exec)));
    CHECK_EQ(book.shares_at('B', kMid), 60u);
    CHECK_EQ(book.volume(), 40u);

    // 'C': ref @11, shares @19, match @23, printable @31, price @32
    std::vector<uint8_t> cexec = header('C');
    append(cexec, be64(1));
    append(cexec, be32(10));
    append(cexec, be64(8));
    cexec.push_back('N');                       // non-printable
    append(cexec, be32(static_cast<uint32_t>(kMid)));
    CHECK_EQ(cexec.size(), 36u);
    CHECK(apply(book, 'C', msg(cexec)));
    CHECK_EQ(book.shares_at('B', kMid), 50u);
    CHECK_EQ(book.volume(), 40u);               // unchanged: not printable

    // 'U': orig @11, new @19, shares @27, price @31
    std::vector<uint8_t> rep = header('U');
    append(rep, be64(1));
    append(rep, be64(2));
    append(rep, be32(25));
    append(rep, be32(static_cast<uint32_t>(kMid - kTick)));
    CHECK_EQ(rep.size(), 35u);
    CHECK(apply(book, 'U', msg(rep)));
    CHECK(book.find(1) == nullptr);
    CHECK_EQ(book.shares_at('B', kMid - kTick), 25u);

    // 'H' and 'S' carry state but mutate nothing.
    std::vector<uint8_t> halt = header('H');
    for (int i = 0; i < 8; ++i) halt.push_back(' ');
    halt.push_back('H');
    halt.push_back(' ');
    for (int i = 0; i < 4; ++i) halt.push_back(' ');
    CHECK_EQ(halt.size(), 25u);
    CHECK(!apply(book, 'H', msg(halt)));
    CHECK_EQ(book.trading_state(), 'H');

    CHECK(!modelled('Y'));
    CHECK(modelled('A'));
}

}  // namespace

int main() {
    test_pool();
    test_level_fifo();
    test_refmap_basics();
    test_refmap_collision_chain();
    test_refmap_churn_against_reference();
    test_best_price_cursor();
    test_overflow_band();
    test_top_ordering_and_depth();
    test_mutation_semantics();
    test_replace_loses_priority_and_inherits_side();
    test_unknown_refs_and_untouched_book();
    test_dispatch_matches_the_wire();
    return REPORT();
}
