// test_restart — coming back after the process died, and the halt invariant
// that is only true some of the time.
//
// The property the snapshot has to deliver:
//
//     snapshot at message N, restart, replay from N
//         == a process that ran continuously through N
//
// Not "close to". Identical — same orders, same queue positions, same tape.
// Anything less means a restarted process quietly disagrees with the one it
// replaced, and the disagreement is invisible until it costs money.
#include <cstdint>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/recover/halt.hpp"
#include "itchbook/recover/snapshot.hpp"
#include "tests/check.hpp"

using namespace itchbook;
using namespace itchbook::recover;

namespace {

// The book's full state as a string, so a failure prints what differs instead
// of just saying that something does. Queue order is included on purpose: it
// is the thing a careless snapshot loses and no level-based check notices.
std::string fingerprint(const book::Book& b) {
    std::string s;
    for (char side : {'B', 'S'}) {
        b.for_each_order(side, [&](const book::Order& o) {
            s += side;
            s += ':' + std::to_string(o.ref) + '@' + std::to_string(o.price) + 'x' +
                 std::to_string(o.shares) + ' ';
        });
    }
    s += "| vol=" + std::to_string(b.volume());
    s += " trades=" + std::to_string(b.trades());
    s += " notional=" + std::to_string(b.notional());
    s += " hi=" + std::to_string(b.high());
    s += " lo=" + std::to_string(b.low());
    s += " unknown=" + std::to_string(b.unknown_ref());
    return s;
}

// A deterministic stream of book operations, so "replay from N" is well defined.
struct Op {
    char kind;      // 'A' add, 'E' execute, 'D' delete, 'X' cancel, 'P' hidden trade
    uint64_t ref;
    char side;
    int32_t price;
    uint32_t shares;
};

std::vector<Op> make_ops() {
    std::vector<Op> ops;
    uint64_t ref = 1;
    // Several orders at each of several prices, so every level has a real FIFO
    // with more than one order in it — a queue of length one cannot be
    // scrambled and would let a broken snapshot pass.
    for (int level = 0; level < 6; ++level) {
        for (int k = 0; k < 4; ++k) {
            ops.push_back({'A', ref++, 'B', 1000000 - level * 100,
                           static_cast<uint32_t>(100 + 10 * k)});
            ops.push_back({'A', ref++, 'S', 1000100 + level * 100,
                           static_cast<uint32_t>(200 + 10 * k)});
        }
    }
    ops.push_back({'E', 1, 'B', 0, 50});
    ops.push_back({'P', 0, 'B', 1000050, 300});
    ops.push_back({'X', 3, 'B', 0, 20});
    ops.push_back({'D', 5, 'B', 0, 0});
    ops.push_back({'E', 2, 'S', 0, 200});
    ops.push_back({'A', ref++, 'B', 1000000, 999});
    ops.push_back({'E', 7, 'B', 0, 110});
    ops.push_back({'D', 9, 'B', 0, 0});
    ops.push_back({'A', ref++, 'S', 1000100, 777});
    ops.push_back({'E', 4, 'S', 0, 100});
    return ops;
}

void apply(book::Book* b, const Op& op) {
    switch (op.kind) {
        case 'A': b->add(op.ref, op.side, op.price, op.shares); break;
        case 'E': b->execute(op.ref, op.shares); break;
        case 'X': b->cancel(op.ref, op.shares); break;
        case 'D': b->remove(op.ref); break;
        case 'P': b->trade(op.price, op.shares); break;
        default: break;
    }
}

void test_restart_equals_a_process_that_never_died() {
    const std::vector<Op> ops = make_ops();

    // Cut at every point in the stream. A snapshot that works at one offset and
    // not another is a snapshot that works by luck.
    for (size_t cut = 1; cut < ops.size(); ++cut) {
        book::Book continuous;
        for (const Op& op : ops) apply(&continuous, op);

        book::Book before;
        for (size_t i = 0; i < cut; ++i) apply(&before, ops[i]);
        const Snapshot snap = capture(before, cut, 34200000000000ULL + cut);

        // The restarted process: fresh book, restore, replay the remainder.
        book::Book after;
        restore(&after, snap);
        for (size_t i = cut; i < ops.size(); ++i) apply(&after, ops[i]);

        CHECK_STR(fingerprint(after), fingerprint(continuous));
    }
}

void test_a_snapshot_survives_a_trip_through_bytes() {
    // The in-memory capture/restore path could be right while the file format
    // silently drops a field, so the serialized round trip is checked
    // separately rather than assumed.
    book::Book b;
    for (const Op& op : make_ops()) apply(&b, op);
    const Snapshot snap = capture(b, 12345, 34200000000001ULL);

    const std::vector<uint8_t> bytes = serialize(snap);
    Snapshot back;
    CHECK(deserialize(bytes.data(), bytes.size(), &back));
    CHECK_EQ(back.next_sequence, 12345u);
    CHECK_EQ(back.last_timestamp, 34200000000001ULL);
    CHECK_EQ(back.orders.size(), snap.orders.size());

    book::Book restored;
    restore(&restored, back);
    CHECK_STR(fingerprint(restored), fingerprint(b));
}

void test_queue_priority_survives_the_round_trip() {
    // The bug this whole file is arranged around. A snapshot that dumps the ref
    // map in hash order restores the right levels, the right shares, and
    // scrambled priority — which every level-based check passes.
    book::Book b;
    b.add(10, 'B', 1000000, 100);
    b.add(11, 'B', 1000000, 100);
    b.add(12, 'B', 1000000, 100);
    b.add(13, 'B', 1000000, 100);

    book::Book restored;
    restore(&restored, capture(b, 1, 1));

    std::string order;
    restored.for_each_order('B', [&](const book::Order& o) {
        order += std::to_string(o.ref) + ' ';
    });
    CHECK_STR(order, std::string("10 11 12 13 "));

    // ...and the FIFO behaves, not just prints, correctly: an execution takes
    // the oldest order first.
    restored.execute(10, 100);
    std::string after;
    restored.for_each_order('B', [&](const book::Order& o) {
        after += std::to_string(o.ref) + ' ';
    });
    CHECK_STR(after, std::string("11 12 13 "));
}

void test_a_truncated_snapshot_is_refused_not_half_loaded() {
    // A partial restore produces a book that looks entirely plausible. Refusing
    // to start is the correct outcome; starting with half a book is the failure
    // this phase is named after.
    book::Book b;
    for (const Op& op : make_ops()) apply(&b, op);
    std::vector<uint8_t> bytes = serialize(capture(b, 1, 1));

    Snapshot out;
    for (size_t keep : {size_t{0}, size_t{4}, bytes.size() / 2, bytes.size() - 1}) {
        CHECK(!deserialize(bytes.data(), keep, &out));
    }
    // A wrong version is refused rather than guessed at: reading a v2 snapshot
    // with v1 code produces a book that is subtly wrong and does not say so.
    bytes[4] = 0x7f;
    CHECK(!deserialize(bytes.data(), bytes.size(), &out));
}

// ---- halts ------------------------------------------------------------------

void test_a_crossed_book_is_legal_while_halted_and_a_defect_while_trading() {
    // The conditional invariant. "bid < ask" is the first thing anyone asserts
    // about a book and it is false during every halt, because the book keeps
    // accepting orders and nothing executes to clear them.
    HaltTracker t;
    book::Book b;
    const uint64_t ts = 34200000000000ULL;

    t.on_trading_action('T', ts);
    b.add(1, 'B', 1000000, 100);
    b.add(2, 'S', 1000200, 100);
    t.observe(b, ts + 1);
    CHECK_EQ(t.stats().crossed_while_trading, 0u);

    // Halt, then cross the book. Legal: this is what a reopening period does.
    t.on_trading_action('H', ts + 100);
    b.add(3, 'B', 1000500, 100);       // bid 1000500 above ask 1000200
    t.observe(b, ts + 200);
    CHECK(b.strictly_crossed());
    CHECK_EQ(t.stats().crossed_while_halted, 1u);
    CHECK_EQ(t.stats().crossed_while_trading, 0u);
    CHECK_EQ(t.stats().deepest_cross_while_halted, 300);
    CHECK(!t.is_tradable());

    // The same crossed book while trading is a defect, and must be counted as
    // one. Without the session state these two observations are identical.
    t.on_trading_action('T', ts + 300);
    t.observe(b, ts + 400);
    CHECK_EQ(t.stats().crossed_while_trading, 1u);
    CHECK_EQ(t.stats().crossed_at_resume, 1u);   // the auction did not clear it
    CHECK_EQ(t.stats().halts, 1u);
    CHECK_EQ(t.stats().resumes, 1u);
}

void test_quotation_only_does_not_trade() {
    // 'Q' is the state a symbol sits in while its reopening auction is being
    // built. Treating it as tradable is how a backtest fills orders in a halt.
    HaltTracker t;
    t.on_trading_action('Q', 1000);
    CHECK(!t.is_tradable());
    CHECK_EQ(static_cast<int>(t.session()), static_cast<int>(Session::QuotationOnly));
    t.on_trading_action('P', 2000);
    CHECK(!t.is_tradable());
    t.on_trading_action('T', 3000);
    CHECK(t.is_tradable());
    // ZERO halts, and this is the case that matters for phase 7. The symbol was
    // never observed trading before it was halted — which is exactly what a
    // process that restarted during a halt sees. It did not witness a halt, so
    // counting one would be a restarted process inventing an event that
    // happened before it existed. The resume it does witness is real.
    CHECK_EQ(t.stats().halts, 0u);
    CHECK_EQ(t.stats().resumes, 1u);
    // Q and P are both non-trading, so moving between them is not a halt
    // either.
    CHECK_EQ(static_cast<int>(t.session()), static_cast<int>(Session::Trading));
}

void test_halted_time_is_accumulated() {
    HaltTracker t;
    const uint64_t s = 1000000000ULL;      // 1s in ns
    t.on_trading_action('T', 0);
    t.observe_at(10 * s);
    t.on_trading_action('H', 10 * s);
    t.observe_at(25 * s);
    t.on_trading_action('T', 25 * s);
    t.observe_at(40 * s);
    CHECK_EQ(t.stats().halted_ns, 15 * s);
}

void test_the_halt_cross_is_recognised() {
    HaltTracker t;
    t.on_trading_action('H', 100);
    t.on_cross('H', 200);        // the reopening auction
    t.on_cross('O', 300);        // an opening cross is a different thing
    t.on_trading_action('T', 400);
    CHECK_EQ(t.stats().halt_crosses, 1u);
}

}  // namespace

int main() {
    test_restart_equals_a_process_that_never_died();
    test_a_snapshot_survives_a_trip_through_bytes();
    test_queue_priority_survives_the_round_trip();
    test_a_truncated_snapshot_is_refused_not_half_loaded();
    test_a_crossed_book_is_legal_while_halted_and_a_defect_while_trading();
    test_quotation_only_does_not_trade();
    test_halted_time_is_accumulated();
    test_the_halt_cross_is_recognised();
    return REPORT();
}
