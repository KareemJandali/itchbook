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
#include "itchbook/emit/sink.hpp"
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

std::vector<uint8_t> add_f(uint64_t ref, char side, uint32_t shares, int32_t px,
                           const char* mpid) {
    auto m = head('F', 40);
    put64(m.data() + 11, ref);
    m[19] = uint8_t(side);
    put32(m.data() + 20, shares);
    std::memcpy(m.data() + 24, "TEST    ", 8);
    put32(m.data() + 32, uint32_t(px));
    std::memcpy(m.data() + 36, mpid, 4);
    return m;
}

std::vector<uint8_t> cross_q(uint64_t shares, int32_t px, char type) {
    auto m = head('Q', 40);
    put64(m.data() + 11, shares);
    std::memcpy(m.data() + 19, "TEST    ", 8);
    put32(m.data() + 27, uint32_t(px));
    put64(m.data() + 31, 77);
    m[39] = uint8_t(type);
    return m;
}

std::vector<uint8_t> sys_event(char code) {
    auto m = head('S', 12);
    m[11] = uint8_t(code);
    return m;
}

std::vector<uint8_t> halt(char state) {
    auto m = head('H', 25);
    std::memcpy(m.data() + 11, "TEST    ", 8);
    m[19] = uint8_t(state);
    m[20] = ' ';
    std::memcpy(m.data() + 21, "REAS", 4);
    return m;
}

std::vector<uint8_t> op_halt(char action) {
    auto m = head('h', 21);
    std::memcpy(m.data() + 11, "TEST    ", 8);
    m[19] = 'Q';
    m[20] = uint8_t(action);
    return m;
}

std::vector<uint8_t> mwcb(char level) {
    auto m = head('W', 12);
    m[11] = uint8_t(level);
    return m;
}

std::vector<uint8_t> broken(uint64_t match) {
    auto m = head('B', 19);
    put64(m.data() + 11, match);
    return m;
}

std::vector<uint8_t> directory() {
    auto m = head('R', 39);
    std::memcpy(m.data() + 11, "TEST    ", 8);
    m[19] = 'Q';
    m[20] = 'N';
    put32(m.data() + 21, 100);
    // The 14 bytes this project does not decode. Non-zero on purpose: the
    // emitter relays them, and a zeroing emitter showed up as 8,906
    // byte-differences on a real day.
    for (size_t i = 25; i < 39; ++i) m[i] = uint8_t('A' + (i % 7));
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
        CHECK_EQ(b->volume(), uint64_t{100});   // 60 + 40, summing to the message
        // TWO prints, not one. This assertion said 1 when the test was written
        // for 12.1, matching a replayer that recorded one print per input
        // execution. That was wrong and no gate could see it: the published
        // feed describes this as two 'E' messages, a subscriber replaying them
        // calls Book::execute twice, and the exchange's own book would have
        // disagreed with its subscriber's by one trade. The 12.1 gate runs at
        // zero strategy orders, where there is exactly one fill per execution
        // and both spellings agree -- so the test pinned the defect instead of
        // catching it.
        CHECK_EQ(b->trades(), uint64_t{2});
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

// ---- 7. the publisher round-trips every type it models ----------------------
//
// P1 checks this on 268 million real messages, but only on the types a real
// file happens to contain. Three -- 'h', 'W', 'B' -- appear in no file in hand,
// so on the real day they are not published because they are not present, and
// their encoders would rot unnoticed. make_sample.py makes the same point about
// its own builders.
void test_emission_round_trip() {
    itchbook::book::BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    itchbook::emit::BufferSink buf;
    r.set_sink(&buf);

    const std::vector<std::vector<uint8_t>> msgs = {
        add(1, 'B', 500, kPx), add_f(2, 'S', 300, kPx + 100, "MPID"),
        exec(1, 100), exec_px(2, 50, kPx + 200, false),
        cancel(1, 50), replace(1, 3, 200, kPx - 100), del(3),
        trade_p(700, kPx), cross_q(1000, kPx, 'O'),
        sys_event('Q'), halt('T'), op_halt('H'), mwcb('1'), broken(42),
        directory(),
    };
    // A NON-ZERO tracking number on every one of them. Both generated feeds
    // and every builder above default it to zero -- make_sample.py header()
    // hard-codes it -- so an emitter that reissued tracking as 0 instead of
    // carrying the field across was invisible to the queue feed, the bench feed
    // AND this test until the number was made non-zero. It is 2.9% non-zero on
    // a real day, so only the licensed-data run would ever have caught it.
    std::vector<std::vector<uint8_t>> stamped = msgs;
    uint16_t trk = 1;
    for (auto& m : stamped) {
        m[3] = uint8_t(trk >> 8);
        m[4] = uint8_t(trk);
        trk = uint16_t(trk * 7 + 11);
    }
    for (const auto& m : stamped) {
        buf.clear();
        r.apply(char(m[0]), m.data());
        // Exactly one published message per input, and byte-identical to it:
        // the header is carried across and every body field is re-encoded, so
        // a wrong offset or endianness shows up here.
        CHECK_EQ(buf.count(), size_t{1});
        if (buf.count() == 1) {
            CHECK_EQ(buf.len(0), m.size());
            if (buf.len(0) == m.size()) {
                CHECK(std::memcmp(buf.at(0), m.data(), m.size()) == 0);
            }
        }
    }
}

// ---- 8. a split fill publishes TWO executions, and they add up --------------
//
// The path P1 cannot reach: with zero strategy orders there is exactly one fill
// per execution, so the multi-fill emission never runs on the real day. An
// adversarial review of the design found a real defect in here that the gate
// was structurally blind to -- the exchange recorded ONE print while its own
// published feed described TWO, so its book and its subscriber's book would
// have parted company by one trade per split fill.
void test_split_fill_emits_two() {
    itchbook::book::BookSet spl(1u << 14, 100, 20, 64);
    SplitReplayer r(spl);
    itchbook::emit::BufferSink buf;
    r.set_sink(&buf);

    const uint64_t sref = kStrategyRefBit | 5;
    spl.at(kLocate).add(sref, 'B', kPx, 60);      // strategy order, ahead
    const auto a = add(50, 'B', 100, kPx);
    const auto e = exec(50, 100);
    r.apply('A', a.data());
    buf.clear();
    r.apply('E', e.data());

    // Two executions, in queue order: the strategy order first.
    CHECK_EQ(buf.count(), size_t{2});
    if (buf.count() != 2) return;
    CHECK_EQ(char(buf.at(0)[0]), 'E');
    CHECK_EQ(char(buf.at(1)[0]), 'E');
    CHECK_EQ(itchbook::itch::order_executed::ref(buf.at(0)), sref);
    CHECK_EQ(itchbook::itch::order_executed::executed_shares(buf.at(0)), uint32_t{60});
    CHECK_EQ(itchbook::itch::order_executed::ref(buf.at(1)), uint64_t{50});
    CHECK_EQ(itchbook::itch::order_executed::executed_shares(buf.at(1)), uint32_t{40});
    // One incoming order walking two makers is ONE match event, and a real
    // venue gives its executions the same match number.
    CHECK_EQ(itchbook::itch::order_executed::match_number(buf.at(0)),
             itchbook::itch::order_executed::match_number(buf.at(1)));

    // The exchange's own book.
    const itchbook::book::Book* x = spl.peek(kLocate);
    CHECK(x != nullptr);
    if (x == nullptr) return;
    CHECK_EQ(x->volume(), uint64_t{100});
    CHECK_EQ(x->trades(), uint64_t{2});     // two prints, because two fills
    CHECK_EQ(x->resting_shares(), uint64_t{60});

    // A SUBSCRIBER rebuilding from the published feed must land in the same
    // place. It has to have been told about the strategy order first -- that is
    // the gateway's Accepted message in 12.5, and here it is applied directly.
    itchbook::book::BookSet sub(1u << 14, 100, 20, 64);
    sub.at(kLocate).add(sref, 'B', kPx, 60);
    itchbook::book::apply(sub, 'A', a.data());
    for (size_t i = 0; i < buf.count(); ++i) {
        itchbook::book::apply(sub, char(buf.at(i)[0]), buf.at(i));
    }
    const itchbook::book::Book* y = sub.peek(kLocate);
    CHECK(y != nullptr);
    if (y == nullptr) return;
    CHECK_EQ(y->volume(), x->volume());
    CHECK_EQ(y->trades(), x->trades());
    CHECK_EQ(y->resting_shares(), x->resting_shares());
    CHECK_EQ(y->resting_orders(), x->resting_orders());
    CHECK_EQ(y->notional(), x->notional());
}

}  // namespace

int main() {
    test_equivalence();
    test_over_execution();
    test_historical_ahead_skipped();
    test_strategy_ahead_fills();
    test_partition();
    test_classification();
    test_emission_round_trip();
    test_split_fill_emits_two();
    return REPORT();
}
