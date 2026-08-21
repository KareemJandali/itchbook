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
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/recover/halt.hpp"
#include "itchbook/recover/snapshot.hpp"
#include "itchbook/recover/strategy_snapshot.hpp"
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

// ---- the other half: position and open orders ------------------------------
//
// The market book restoring perfectly while our own position and resting
// orders come back empty is not a partial success, it is the silent-wrongness
// failure wearing the recovery mechanism's clothes: the replay continues,
// every invariant holds, and the run reports a P&L for a strategy that spent
// the afternoon flat.

std::string fingerprint(const sim::Ledger& l, const sim::QueueModel& q) {
    std::string s = "cash=" + std::to_string(l.cash()) +
                    " pos=" + std::to_string(l.position()) +
                    " fees=" + std::to_string(l.fees()) +
                    " edge=" + std::to_string(l.edge()) +
                    " fills=" + std::to_string(l.fills().size()) +
                    " nomid=" + std::to_string(l.fills_without_mid()) +
                    " shares=" + std::to_string(l.shares_traded()) + " |";
    // Queue position is in here on purpose. An order restored with the right
    // price and size but ahead = 0 sits at the front of a queue it never
    // reached: it fills on the next print and books money that was never made.
    for (const sim::Entry& e : q.entries()) {
        s += " #" + std::to_string(e.id) + '@' + std::to_string(e.price) +
             " disp=" + std::to_string(e.display) +
             " hid=" + std::to_string(e.hidden) +
             " ahead=" + std::to_string(e.ahead) +
             " ahead0=" + std::to_string(e.ahead0) +
             " refr=" + std::to_string(e.refreshes) +
             " live=" + std::to_string(static_cast<int>(e.live));
    }
    s += " | clamps=" + std::to_string(q.clamp_events()) +
         " anomalies=" + std::to_string(q.priority_anomalies()) +
         " naive_hidden=" + std::to_string(q.naive_hidden_fills()) +
         " tradable=" + std::to_string(static_cast<int>(q.tradable()));
    return s;
}

// A lane with genuinely non-trivial state: real fills in the ledger, several
// resting orders including an iceberg, and a queue the orders are partway
// through. Restoring a lane that is empty proves nothing.
void build_lane(sim::Ledger* l, sim::QueueModel* q) {
    book::Book b(100, 20, 1024);
    b.add(1, 'B', 100'0000, 500);
    b.add(2, 'B', 100'0000, 300);
    b.add(3, 'S', 100'2000, 400);

    q->place(b, 10, sim::Side::Buy, 100'0000, 200, 0, 1'000);
    q->place(b, 11, sim::Side::Buy, 100'0000, 400, 100, 2'000);   // iceberg
    q->place(b, 12, sim::Side::Sell, 100'2000, 150, 0, 3'000);

    const sim::Mid mid{200'2000, sim::MidStatus::Ok};
    for (int i = 0; i < 4; ++i) {
        sim::SimFill f;
        f.order_id = 10;
        f.side = (i % 2 == 0) ? sim::Side::Buy : sim::Side::Sell;
        f.price = 100'0000 + i * 100;
        f.shares = 50u + static_cast<uint32_t>(i);
        f.ts = 5'000 + static_cast<uint64_t>(i) * 1'000;
        f.liquidity = sim::Liquidity::Added;
        // One fill with no usable mid, because the count of those is reported
        // and a restore that loses it changes the reconciliation.
        l->on_fill(f, i == 3 ? sim::Mid{} : mid);
    }
}

void test_position_and_open_orders_survive_a_restart() {
    sim::QueueConfig cfg;
    cfg.model = sim::Model::Mbo;      // the model that also carries an ahead-set
    sim::Ledger live;
    sim::QueueModel live_q(cfg);
    build_lane(&live, &live_q);

    const std::string before = fingerprint(live, live_q);

    StrategySnapshot snap{live.state(), live_q.state_snapshot()};
    const std::vector<uint8_t> bytes = serialize(snap);
    CHECK(!bytes.empty());

    StrategySnapshot back;
    CHECK(deserialize_strategy(bytes.data(), bytes.size(), &back));

    // Fresh objects, as a restarted process would have.
    sim::Ledger restored;
    sim::QueueModel restored_q(cfg);
    restored.restore(back.ledger);
    restored_q.restore(back.queue);

    CHECK_STR(fingerprint(restored, restored_q), before);
}

void test_a_restored_lane_keeps_going_the_same_way() {
    // Round-tripping the state is not the property. The property is that the
    // restored lane BEHAVES like the one it replaced, so both are driven
    // through the same subsequent fill and must still agree.
    sim::QueueConfig cfg;
    cfg.model = sim::Model::Pessimistic;
    sim::Ledger a, b;
    sim::QueueModel qa(cfg), qb(cfg);
    build_lane(&a, &qa);
    build_lane(&b, &qb);

    StrategySnapshot snap{b.state(), qb.state_snapshot()};
    const std::vector<uint8_t> bytes = serialize(snap);
    StrategySnapshot back;
    CHECK(deserialize_strategy(bytes.data(), bytes.size(), &back));
    sim::Ledger restored;
    sim::QueueModel restored_q(cfg);
    restored.restore(back.ledger);
    restored_q.restore(back.queue);

    sim::SimFill f;
    f.order_id = 11;
    f.side = sim::Side::Buy;
    f.price = 100'0000;
    f.shares = 75;
    f.ts = 9'000;
    const sim::Mid mid{200'0000, sim::MidStatus::Ok};
    a.on_fill(f, mid);
    restored.on_fill(f, mid);

    CHECK_STR(fingerprint(restored, restored_q), fingerprint(a, qa));
}

void test_every_field_survives_the_round_trip() {
    // The behavioural tests above exercise a lane the simulator built, which
    // means any field that lane happens to leave at zero is a field the
    // serializer can drop without anything noticing. Writing this serializer
    // I dropped naive_hidden_fills on the read side, and it passed, because
    // the mbo lane never sets it. So: every field gets a distinct non-zero
    // value, and any dropped or transposed one shows up as a mismatch.
    StrategySnapshot in;
    in.ledger.cash = -123456789;
    in.ledger.position = -4242;
    in.ledger.fees_total = 987654;
    in.ledger.two_edge = -31337;
    in.ledger.fills_without_mid = 7;
    sim::LedgerFill f;
    f.ts = 111; f.side = sim::Side::Sell; f.price = 100'2500; f.shares = 33;
    f.two_mid = 200'5000; f.mid_ok = true; f.fee = -55;
    f.liquidity = sim::Liquidity::Removed;
    in.ledger.fills.push_back(f);
    f.ts = 222; f.side = sim::Side::Buy; f.mid_ok = false;
    f.liquidity = sim::Liquidity::Added;
    in.ledger.fills.push_back(f);

    sim::Entry e;
    e.id = 909; e.side = sim::Side::Sell; e.price = 99'7500; e.display = 12;
    e.slice_size = 12; e.hidden = 88; e.ahead = 4321; e.ahead0 = 5000;
    e.arrived_ns = 777'000; e.refreshes = 3; e.live = true;
    in.queue.entries.push_back(e);
    e.id = 910; e.live = false; e.ahead = 0;
    in.queue.entries.push_back(e);

    in.queue.ahead_sets.push_back({909, {{5, 50}, {6, 60}}});
    in.queue.ahead_sets.push_back({910, {{7, 70}}});
    in.queue.state = 'Q';
    in.queue.tradable = true;
    in.queue.clamp_events = 11;
    in.queue.clamp_shares = 22;
    in.queue.priority_anomalies = 33;
    in.queue.anomaly_shares = 44;
    in.queue.hidden_inside_shares = 55;
    in.queue.naive_hidden_fills = 66;
    in.queue.naive_hidden_shares = 77;

    const std::vector<uint8_t> bytes = serialize(in);
    StrategySnapshot out;
    CHECK(deserialize_strategy(bytes.data(), bytes.size(), &out));

    CHECK_EQ(out.ledger.cash, in.ledger.cash);
    CHECK_EQ(out.ledger.position, in.ledger.position);
    CHECK_EQ(out.ledger.fees_total, in.ledger.fees_total);
    CHECK_EQ(out.ledger.two_edge, in.ledger.two_edge);
    CHECK_EQ(out.ledger.fills_without_mid, in.ledger.fills_without_mid);
    CHECK_EQ(out.ledger.fills.size(), in.ledger.fills.size());
    for (size_t i = 0; i < in.ledger.fills.size(); ++i) {
        const sim::LedgerFill& a = in.ledger.fills[i];
        const sim::LedgerFill& b = out.ledger.fills[i];
        CHECK_EQ(b.ts, a.ts);
        CHECK(b.side == a.side);
        CHECK_EQ(b.price, a.price);
        CHECK_EQ(b.shares, a.shares);
        CHECK_EQ(b.two_mid, a.two_mid);
        CHECK(b.mid_ok == a.mid_ok);
        CHECK_EQ(b.fee, a.fee);
        CHECK(b.liquidity == a.liquidity);
    }

    CHECK_EQ(out.queue.entries.size(), in.queue.entries.size());
    for (size_t i = 0; i < in.queue.entries.size(); ++i) {
        const sim::Entry& a = in.queue.entries[i];
        const sim::Entry& b = out.queue.entries[i];
        CHECK_EQ(b.id, a.id);
        CHECK(b.side == a.side);
        CHECK_EQ(b.price, a.price);
        CHECK_EQ(b.display, a.display);
        CHECK_EQ(b.slice_size, a.slice_size);
        CHECK_EQ(b.hidden, a.hidden);
        CHECK_EQ(b.ahead, a.ahead);
        CHECK_EQ(b.ahead0, a.ahead0);
        CHECK_EQ(b.arrived_ns, a.arrived_ns);
        CHECK_EQ(b.refreshes, a.refreshes);
        CHECK(b.live == a.live);
    }

    CHECK(out.queue.ahead_sets == in.queue.ahead_sets);
    CHECK(out.queue.state == in.queue.state);
    CHECK(out.queue.tradable == in.queue.tradable);
    CHECK_EQ(out.queue.clamp_events, in.queue.clamp_events);
    CHECK_EQ(out.queue.clamp_shares, in.queue.clamp_shares);
    CHECK_EQ(out.queue.priority_anomalies, in.queue.priority_anomalies);
    CHECK_EQ(out.queue.anomaly_shares, in.queue.anomaly_shares);
    CHECK_EQ(out.queue.hidden_inside_shares, in.queue.hidden_inside_shares);
    CHECK_EQ(out.queue.naive_hidden_fills, in.queue.naive_hidden_fills);
    CHECK_EQ(out.queue.naive_hidden_shares, in.queue.naive_hidden_shares);
}

void test_a_truncated_strategy_snapshot_is_refused() {
    sim::Ledger l;
    sim::QueueModel q;
    build_lane(&l, &q);
    const std::vector<uint8_t> bytes = serialize(StrategySnapshot{l.state(), q.state_snapshot()});

    // Every prefix must be refused rather than half-loaded. Half-loaded is the
    // dangerous outcome: it restores a position with some of its orders.
    for (size_t n = 0; n < bytes.size(); ++n) {
        StrategySnapshot back;
        CHECK(!deserialize_strategy(bytes.data(), n, &back));
    }
    StrategySnapshot back;
    CHECK(deserialize_strategy(bytes.data(), bytes.size(), &back));

    // ...and a corrupt length prefix must not be believed. Reserving on an
    // unchecked count is how a truncated file becomes an out-of-memory abort
    // instead of a clean refusal, so this claims 2^64-1 fills in a file too
    // short to hold ten. The offset is derived from the layout rather than
    // written as a number, because a field added above it would silently move
    // this probe onto something else and the test would pass for free.
    const size_t fill_count_off = sizeof(uint32_t) * 2      // magic, version
                                + sizeof(sim::Money)        // cash
                                + sizeof(int64_t)           // position
                                + sizeof(sim::Money)        // fees_total
                                + sizeof(int64_t)           // two_edge
                                + sizeof(uint64_t);         // fills_without_mid
    std::vector<uint8_t> bad = bytes;
    CHECK(bad.size() > fill_count_off + 8);
    for (size_t i = 0; i < 8; ++i) bad[fill_count_off + i] = 0xff;
    StrategySnapshot huge;
    CHECK(!deserialize_strategy(bad.data(), bad.size(), &huge));

    // The nastier case, and the one a bytes-only bound lets through: a count
    // equal to the number of BYTES left. A LedgerFill is 35 bytes on the wire
    // and 56 in memory, so `n > len - off` passes and the reserve still asks
    // for 1.6x the file — and 1.6x of a large file is an out-of-memory abort
    // out of an inline header nobody catches. The bound has to divide by the
    // wire record size, which is what snapshot.hpp already did.
    std::vector<uint8_t> padded(64 * 1024, 0);
    std::memcpy(padded.data(), bytes.data(), std::min(bytes.size(), padded.size()));
    const uint64_t claim = padded.size() - fill_count_off - 8;
    std::memcpy(padded.data() + fill_count_off, &claim, sizeof(claim));
    StrategySnapshot plausible;
    CHECK(!deserialize_strategy(padded.data(), padded.size(), &plausible));
    CHECK_EQ(plausible.ledger.fills.capacity(), 0u);
}

int main() {
    test_restart_equals_a_process_that_never_died();
    test_a_snapshot_survives_a_trip_through_bytes();
    test_queue_priority_survives_the_round_trip();
    test_a_truncated_snapshot_is_refused_not_half_loaded();
    test_a_crossed_book_is_legal_while_halted_and_a_defect_while_trading();
    test_quotation_only_does_not_trade();
    test_halted_time_is_accumulated();
    test_the_halt_cross_is_recognised();
    test_position_and_open_orders_survive_a_restart();
    test_a_restored_lane_keeps_going_the_same_way();
    test_every_field_survives_the_round_trip();
    test_a_truncated_strategy_snapshot_is_refused();
    return REPORT();
}
