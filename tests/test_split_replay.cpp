//
// test_split_replay.cpp — phase 12.1's split replayer, on hand-built messages.
//
// The full-day gate proves equivalence on 268 million real messages, which is
// the strongest evidence available and still misses the cases a real feed does
// not happen to contain. A mutation test found exactly one such hole: clamping
// the tape print to the resting order's size survives both the generated feeds
// AND the real day, because no well-formed feed executes more shares than the
// order is displaying. It is reachable, it changes reported volume, and the
// only way to pin it is to write the message by hand. That is test 2 below.
//
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/replay/split.hpp"
#include "tests/check.hpp"

namespace {

using itchbook::book::BookSet;
using itchbook::replay::kStrategyRefBit;
using itchbook::replay::SplitReplayer;

constexpr uint16_t kLocate = 7;
constexpr int32_t kPx = 1000000;   // $100.0000

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}

// Common header: type, stock locate, tracking number, 6-byte timestamp.
std::vector<uint8_t> head(char type, size_t len) {
    std::vector<uint8_t> m(len, 0);
    m[0] = uint8_t(type);
    put16(m.data() + 1, kLocate);
    return m;
}

std::vector<uint8_t> add(uint64_t ref, char side, uint32_t shares, int32_t px) {
    auto m = head('A', 36);
    put64(m.data() + 11, ref);
    m[19] = uint8_t(side);
    put32(m.data() + 20, shares);
    std::memcpy(m.data() + 24, "TEST    ", 8);
    put32(m.data() + 32, uint32_t(px));
    return m;
}

std::vector<uint8_t> exec(uint64_t ref, uint32_t shares) {
    auto m = head('E', 31);
    put64(m.data() + 11, ref);
    put32(m.data() + 19, shares);
    return m;
}

std::vector<uint8_t> exec_px(uint64_t ref, uint32_t shares, int32_t px, bool printable) {
    auto m = head('C', 36);
    put64(m.data() + 11, ref);
    put32(m.data() + 19, shares);
    m[31] = uint8_t(printable ? 'Y' : 'N');
    put32(m.data() + 32, uint32_t(px));
    return m;
}

std::vector<uint8_t> cancel(uint64_t ref, uint32_t shares) {
    auto m = head('X', 23);
    put64(m.data() + 11, ref);
    put32(m.data() + 19, shares);
    return m;
}

std::vector<uint8_t> del(uint64_t ref) {
    auto m = head('D', 19);
    put64(m.data() + 11, ref);
    return m;
}

std::vector<uint8_t> replace(uint64_t orig, uint64_t nref, uint32_t shares, int32_t px) {
    auto m = head('U', 35);
    put64(m.data() + 11, orig);
    put64(m.data() + 19, nref);
    put32(m.data() + 27, shares);
    put32(m.data() + 31, uint32_t(px));
    return m;
}

std::vector<uint8_t> trade_p(uint32_t shares, int32_t px) {
    auto m = head('P', 44);
    put64(m.data() + 11, 1);
    m[19] = uint8_t('B');
    put32(m.data() + 20, shares);
    std::memcpy(m.data() + 24, "TEST    ", 8);
    put32(m.data() + 32, uint32_t(px));
    return m;
}

// Everything a divergence could show up in, compared field by field.
struct Snap {
    size_t orders; uint64_t shares, volume, notional, trades, hidden, cross, unknown, adds;
    int32_t open, high, low, close, bid, ask;
};

Snap snap(const itchbook::book::Book& b) {
    int32_t v = 0;
    Snap s{};
    s.orders = b.resting_orders(); s.shares = b.resting_shares();
    s.volume = b.volume(); s.notional = b.notional(); s.trades = b.trades();
    s.hidden = b.hidden_volume(); s.cross = b.cross_volume();
    s.unknown = b.unknown_ref(); s.adds = b.adds();
    s.open = b.open(); s.high = b.high(); s.low = b.low(); s.close = b.close();
    s.bid = b.best_bid(&v) ? v : -1;
    s.ask = b.best_ask(&v) ? v : -1;
    return s;
}

void check_same(const Snap& a, const Snap& b, const char* what) {
    (void)what;
    CHECK_EQ(a.orders, b.orders);   CHECK_EQ(a.shares, b.shares);
    CHECK_EQ(a.volume, b.volume);   CHECK_EQ(a.notional, b.notional);
    CHECK_EQ(a.trades, b.trades);   CHECK_EQ(a.hidden, b.hidden);
    CHECK_EQ(a.cross, b.cross);     CHECK_EQ(a.unknown, b.unknown);
    CHECK_EQ(a.adds, b.adds);       CHECK_EQ(a.open, b.open);
    CHECK_EQ(a.high, b.high);       CHECK_EQ(a.low, b.low);
    CHECK_EQ(a.close, b.close);     CHECK_EQ(a.bid, b.bid);
    CHECK_EQ(a.ask, b.ask);
}

// Drive both paths over the same messages and require they agree after each.
void both(const std::vector<std::vector<uint8_t>>& msgs, const char* what) {
    BookSet ref(1u << 14, 100, 20, 64);
    BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    for (const auto& m : msgs) {
        const char t = char(m[0]);
        itchbook::book::apply(ref, t, m.data());
        r.apply(t, m.data());
        const itchbook::book::Book* a = ref.peek(kLocate);
        const itchbook::book::Book* b = spl.peek(kLocate);
        CHECK((a == nullptr) == (b == nullptr));
        if (a != nullptr && b != nullptr) check_same(snap(*a), snap(*b), what);
    }
    CHECK(r.partition_held());
}

// ---- 1. the ordinary shapes, applied both ways -------------------------------
void test_equivalence() {
    both({add(1, 'B', 500, kPx), add(2, 'B', 300, kPx), add(3, 'S', 400, kPx + 100),
          exec(1, 200), cancel(2, 100), exec(1, 300), del(3),
          replace(2, 4, 250, kPx - 100), exec(4, 250)}, "ordinary");

    // C and P are applied as state, not aggressed. Both are checked here
    // precisely because the classification table sends them down the other
    // branch than E, and a table is worth exactly what its test is.
    both({add(10, 'B', 100, kPx), add(11, 'B', 100, kPx),
          exec_px(11, 40, kPx + 50, true),    // names the order BEHIND the front
          exec_px(11, 60, kPx + 50, false),   // non-printable: book moves, tape does not
          trade_p(700, kPx + 25)}, "C and P");

    // An execution against a reference no book holds. Phase 9 counts it in
    // unknown_ref; the split path must count it the same way rather than
    // reporting a cleaner sheet than the feed deserves.
    both({add(20, 'S', 100, kPx), exec(999, 50), exec(20, 50)}, "unknown ref");
}

// ---- 2. the mutation the feeds could not kill --------------------------------
//
// execute() hands record_trade the MESSAGE's share count, not the clamped one:
// 250 executed against a 100-share order reports volume 250 and empties the
// book. It is not reachable on a well-formed feed, which is why 268 million
// real messages did not cover it, and it is one line away in the replayer.
void test_over_execution() {
    both({add(30, 'B', 100, kPx), exec(30, 250)}, "over-execution");

    BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    const auto a = add(30, 'B', 100, kPx);
    const auto e = exec(30, 250);
    r.apply('A', a.data());
    r.apply('E', e.data());
    const itchbook::book::Book* b = spl.peek(kLocate);
    CHECK(b != nullptr);
    if (b != nullptr) {
        CHECK_EQ(b->volume(), uint64_t{250});               // the message's count
        CHECK_EQ(b->trades(), uint64_t{1});                 // one print, not two
        CHECK_EQ(b->resting_shares(), uint64_t{0});
        CHECK_EQ(b->resting_orders(), size_t{0});
    }
}

// ---- 3. a historical order ahead is skipped, not eaten -----------------------
//
// The feed is ground truth about WHICH historical order traded. One resting in
// front of the named order demonstrably did not trade here, whatever our
// reconstruction's queue says, so consuming it would invent a fill that history
// contradicts. This happens 10,565 times on 2019-12-30.
void test_historical_ahead_skipped() {
    BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    const auto a1 = add(40, 'B', 100, kPx);   // front of the queue
    const auto a2 = add(41, 'B', 100, kPx);   // behind it
    const auto e = exec(41, 100);             // the feed names the BACK one
    r.apply('A', a1.data());
    r.apply('A', a2.data());
    r.apply('E', e.data());

    const itchbook::book::Book* b = spl.peek(kLocate);
    CHECK(b != nullptr);
    if (b != nullptr) {
        CHECK(b->find(40) != nullptr);                  // untouched
        if (b->find(40) != nullptr) CHECK_EQ(b->find(40)->shares, uint32_t{100});
        CHECK(b->find(41) == nullptr);                  // gone
        CHECK_EQ(b->resting_shares(), uint64_t{100});
        CHECK_EQ(b->volume(), uint64_t{100});
    }
    CHECK_EQ(r.counters().historical_ahead, uint64_t{1});
    CHECK_EQ(r.counters().strategy_shares_taken, uint64_t{0});
}

// ---- 4. a strategy order ahead DOES take the shares --------------------------
//
// The entire point of the split. With the strategy order in front, the
// synthesised aggressor fills it first and the historical order keeps shares
// that history says it lost -- a divergence, stated in section 3 of the design
// doc, and bounded by exactly this quantity.
void test_strategy_ahead_fills() {
    BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    const uint64_t sref = kStrategyRefBit | 5;

    // The strategy order rests first, so it is ahead in the queue.
    spl.at(kLocate).add(sref, 'B', kPx, 60);
    const auto a = add(50, 'B', 100, kPx);
    const auto e = exec(50, 100);
    r.apply('A', a.data());
    r.apply('E', e.data());

    const itchbook::book::Book* b = spl.peek(kLocate);
    CHECK(b != nullptr);
    if (b != nullptr) {
        CHECK(b->find(sref) == nullptr);                 // fully filled
        CHECK(b->find(50) != nullptr);                   // kept 60 of its 100
        if (b->find(50) != nullptr) CHECK_EQ(b->find(50)->shares, uint32_t{60});
        CHECK_EQ(b->volume(), uint64_t{100});            // ONE print of 100
        CHECK_EQ(b->trades(), uint64_t{1});
    }
    CHECK_EQ(r.counters().strategy_shares_taken, uint64_t{60});
    CHECK(r.partition_held());   // a strategy ref is not a violation
}

// ---- 5. the partition, on every reference-bearing message --------------------
void test_partition() {
    {   // an add carrying a high-half reference
        BookSet s(1u << 14, 100, 20, 64);
        SplitReplayer r(s);
        const auto m = add(kStrategyRefBit | 9, 'B', 100, kPx);
        r.apply('A', m.data());
        CHECK_EQ(r.counters().partition_violations, uint64_t{1});
        CHECK(!r.partition_held());
    }
    {   // A REPLACE whose NEW reference is in the high half. This is the one an
        // original_ref-only check misses, and the new reference is the one that
        // actually gets inserted into the map.
        BookSet s(1u << 14, 100, 20, 64);
        SplitReplayer r(s);
        const auto a = add(60, 'B', 100, kPx);
        const auto u = replace(60, kStrategyRefBit | 61, 100, kPx);
        r.apply('A', a.data());
        r.apply('U', u.data());
        CHECK_EQ(r.counters().partition_violations, uint64_t{1});
        CHECK(!r.partition_held());
    }
    {   // and a clean run stays clean
        BookSet s(1u << 14, 100, 20, 64);
        SplitReplayer r(s);
        const auto a = add(70, 'B', 100, kPx);
        const auto e = exec(70, 100);
        r.apply('A', a.data());
        r.apply('E', e.data());
        CHECK_EQ(r.counters().partition_violations, uint64_t{0});
        CHECK(r.partition_held());
    }
}

// ---- 6. the classification table itself --------------------------------------
void test_classification() {
    using itchbook::replay::Treatment;
    using itchbook::replay::treatment_of;
    CHECK(treatment_of('E') == Treatment::Aggressor);
    for (char t : {'A', 'F', 'U', 'X', 'D', 'C', 'P', 'Q', 'R', 'H', 'S', 'h', 'W', 'B'}) {
        CHECK(treatment_of(t) == Treatment::State);
    }
    CHECK(itchbook::replay::is_strategy_ref(kStrategyRefBit));
    CHECK(itchbook::replay::is_strategy_ref(~uint64_t{0}));
    CHECK(!itchbook::replay::is_strategy_ref(0));
    CHECK(!itchbook::replay::is_strategy_ref(kStrategyRefBit - 1));   // the boundary
}

}  // namespace

int main() {
    test_equivalence();
    test_over_execution();
    test_historical_ahead_skipped();
    test_strategy_ahead_fills();
    test_partition();
    test_classification();
    return REPORT();
}
