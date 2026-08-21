// test_recover — the gap policy, and the one thing it is allowed to claim.
//
// The build plan's requirement is "must either produce correct state or halt
// safely. Never silently wrong." The word carrying the weight is *silently*.
// Rebuild-forward does not produce a correct book immediately — it produces a
// SUBSET of the truth, holding no wrong orders and an unknown number of
// missing ones — and the whole point of the policy object is that the state is
// reported rather than assumed. These tests assert on the reporting as much as
// on the arithmetic.
#include <cstdint>

#include "itchbook/book/book.hpp"
#include "itchbook/recover/gap_policy.hpp"
#include "tests/check.hpp"

using namespace itchbook::recover;

namespace {

void test_a_clean_stream_is_trusted() {
    GapTracker t;
    CHECK(t.trusted());
    CHECK(!t.halted());
    for (int i = 0; i < 1000; ++i) t.on_reference(true);
    CHECK(t.trusted());
    CHECK_EQ(t.stats().gaps, 0u);
    // A trusted book must not be counting itself as recovering.
    CHECK_EQ(t.stats().messages_untrusted, 0u);
}

void test_a_gap_stops_the_book_being_trusted() {
    GapTracker t;
    CHECK(t.on_gap(100, 40));       // caller must clear
    CHECK(!t.trusted());
    CHECK_EQ(static_cast<int>(t.state()), static_cast<int>(State::Recovering));
    CHECK_EQ(t.stats().messages_lost, 40u);
    CHECK_EQ(t.stats().rebuilds, 1u);
}

void test_halt_policy_never_rebuilds() {
    // Always correct, never useful past the first loss of the day. It exists so
    // that "stop" is available to anyone who wants it, and so the rebuild path
    // has something to be compared against.
    GapConfig c;
    c.policy = Policy::Halt;
    GapTracker t{c};
    CHECK(!t.on_gap(100, 1));       // caller must NOT clear: nothing resumes
    CHECK(t.halted());
    CHECK_EQ(t.stats().rebuilds, 0u);
    // ...and it stays halted no matter how clean the feed gets afterwards.
    for (int i = 0; i < 100000; ++i) t.on_reference(true);
    CHECK(t.halted());
    CHECK_EQ(t.stats().recoveries, 0u);
}

void test_recovery_needs_an_unbroken_run() {
    // The convergence bar. A single unresolved reference means an order from
    // before the gap is still out there. What has to be true is that the RATE
    // falls, not that a run of perfect messages is never interrupted.
    GapConfig c;
    c.recovery_window = 1600;
    c.recovery_tolerance = 2;
    GapTracker t{c};
    t.on_gap(1, 10);

    // The window has to be full before a rate over it means anything.
    for (int i = 0; i < 1599; ++i) t.on_reference(true);
    CHECK(!t.trusted());
    t.on_reference(true);
    CHECK(t.trusted());
    CHECK_EQ(t.stats().recoveries, 1u);
}

void test_one_straggler_does_not_block_recovery_forever() {
    // The bug real data found, and the reason this is a windowed rate rather
    // than a consecutive run.
    //
    // The first version reset a counter to zero on any unresolved reference.
    // On the synthetic feed that works, because orders churn fast and the
    // pre-gap tail dies out in seconds. On a real MSFT day it never recovers:
    // an order added before the gap can rest for HOURS, and one cancel of one
    // such order wipes out a run that had climbed to nineteen thousand. The
    // book had long since converged; the criterion could not say so.
    GapConfig c;
    c.recovery_window = 1600;
    c.recovery_tolerance = 2;
    GapTracker t{c};
    t.on_gap(1, 10);

    // A long-lived pre-gap order surfaces once every few hundred messages —
    // rare, but never quite absent. Under the old rule this recovered never.
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 400; ++i) t.on_reference(true);
        t.on_reference(false);
    }
    for (int i = 0; i < 1600; ++i) t.on_reference(true);
    CHECK(t.trusted());
}

void test_a_sustained_rate_of_stragglers_does_block_recovery() {
    // The other half. A book still riddled with references it cannot resolve
    // has not converged, and tolerating a rate must not mean tolerating this.
    GapConfig c;
    c.recovery_window = 1600;
    c.recovery_tolerance = 2;
    GapTracker t{c};
    t.on_gap(1, 10);
    for (int i = 0; i < 20000; ++i) t.on_reference(i % 50 != 0);   // 2% unknown
    CHECK(!t.trusted());
    CHECK_EQ(t.stats().recoveries, 0u);
}

void test_a_second_gap_undoes_a_recovery() {
    GapConfig c;
    c.recovery_window = 10;
    c.recovery_tolerance = 0;
    GapTracker t{c};
    t.on_gap(1, 5);
    for (int i = 0; i < 10; ++i) t.on_reference(true);
    CHECK(t.trusted());
    t.on_gap(500, 5);
    CHECK(!t.trusted());
    CHECK_EQ(t.stats().gaps, 2u);
    CHECK_EQ(t.stats().rebuilds, 2u);
}

void test_a_feed_that_keeps_losing_packets_is_not_a_feed_you_are_recovering_from() {
    // A system that resyncs forever looks healthy while being blind. After
    // enough gaps the honest answer stops being "rebuild" and becomes "this
    // link is not delivering the feed".
    GapConfig c;
    c.max_gaps_before_halt = 5;
    c.recovery_window = 1;
    c.recovery_tolerance = 0;
    GapTracker t{c};
    for (int i = 0; i < 5; ++i) {
        CHECK(t.on_gap(static_cast<uint64_t>(i) * 100, 1));
        t.on_reference(true);
    }
    CHECK(t.trusted());
    CHECK(!t.on_gap(9999, 1));      // the sixth: give up
    CHECK(t.halted());
}

void test_clearing_a_book_keeps_the_tape_and_drops_the_orders() {
    // A gap does not make the prints before it un-happen. Volume and VWAP are
    // facts about trades we saw; the resting orders are the thing that can no
    // longer be shown to be correct.
    itchbook::book::Book b;
    b.add(1, 'B', 1000000, 100);
    b.add(2, 'S', 1000200, 200);
    b.execute(1, 50);
    const uint64_t volume_before = b.volume();
    CHECK(volume_before > 0);

    b.clear_orders();
    CHECK_EQ(b.resting_orders(), 0u);
    CHECK_EQ(b.resting_shares(), 0u);
    CHECK_EQ(b.volume(), volume_before);      // the tape survives

    int32_t px = 0;
    CHECK(!b.best_bid(&px));
    CHECK(!b.best_ask(&px));

    // ...and the book still works afterwards. A clear that leaves the
    // structure unusable turns one gap into the end of the session.
    b.add(3, 'B', 999900, 300);
    CHECK(b.best_bid(&px));
    CHECK_EQ(px, 999900);
    CHECK_EQ(b.resting_orders(), 1u);
}

// ---- shared storage --------------------------------------------------------
//
// Phase 9 puts every symbol in one process behind one reference map and one
// pool, because a per-book map at the default size is 16 MB and nine thousand
// of them do not fit in a machine. That makes recovery a different problem: a
// gap on one symbol must not be an outage for the other 8,699, and the old
// clear_orders() -- which dropped the whole Pool and the whole RefMap and built
// new ones -- would have been exactly that. It would not have failed a test
// either, because until now a book was the only thing in its containers.

void test_clearing_one_book_leaves_its_neighbours_untouched() {
    itchbook::book::Storage store;
    itchbook::book::Book a(store, /*locate=*/1);
    itchbook::book::Book b(store, /*locate=*/2);

    a.add(10, 'B', 1000000, 100);
    a.add(11, 'B', 999900, 200);
    a.add(12, 'S', 1000200, 300);
    b.add(20, 'B', 500000, 400);
    b.add(21, 'S', 500100, 500);

    CHECK_EQ(a.resting_orders(), 3u);
    CHECK_EQ(b.resting_orders(), 2u);
    CHECK_EQ(store.pool.live(), 5u);
    CHECK_EQ(store.refs.size(), 5u);

    a.clear_orders();

    // The cleared book is empty...
    CHECK_EQ(a.resting_orders(), 0u);
    CHECK_EQ(a.resting_shares(), 0u);
    CHECK(a.find(10) == nullptr);
    CHECK(a.find(11) == nullptr);
    CHECK(a.find(12) == nullptr);

    // ...and its neighbour has not noticed. This is the assertion the whole
    // refactor exists for: every one of b's references still resolves, to the
    // same order, with the same shares.
    CHECK_EQ(b.resting_orders(), 2u);
    CHECK_EQ(b.resting_shares(), 900u);
    const itchbook::book::Order* b20 = b.find(20);
    const itchbook::book::Order* b21 = b.find(21);
    CHECK(b20 != nullptr);
    CHECK(b21 != nullptr);
    // Guarded, so that a regression here reports every check that failed
    // instead of dereferencing null and taking the run down with it.
    if (b20 != nullptr) CHECK_EQ(b20->shares, 400u);
    if (b21 != nullptr) CHECK_EQ(b21->shares, 500u);
    int32_t px = 0;
    CHECK(b.best_bid(&px));
    CHECK_EQ(px, 500000);

    // Exactly three nodes went back to the shared pool: the cleared book's,
    // and nobody else's. A clear that leaked would leave live() at 5 and a
    // clear that over-freed would corrupt b, and neither shows up in b's own
    // numbers -- only in the pool's.
    CHECK_EQ(store.pool.live(), 2u);
    CHECK_EQ(store.refs.size(), 2u);

    // And the cleared book still works. A gap that leaves a symbol unusable
    // turns one lost packet into the end of that symbol's session.
    a.add(13, 'B', 1000100, 600);
    CHECK_EQ(a.resting_orders(), 1u);
    CHECK(a.best_bid(&px));
    CHECK_EQ(px, 1000100);
    CHECK_EQ(store.pool.live(), 3u);
}

void test_shared_storage_keeps_two_books_apart_under_ordinary_flow() {
    // Not just on recovery: adds, executions and deletes on one book must not
    // move the other's counts, and a shared pool must hand every book its own
    // nodes. Reference uniqueness across the feed is what makes one map legal,
    // so the two books here deliberately never share a reference.
    itchbook::book::Storage store;
    itchbook::book::Book a(store, 1);
    itchbook::book::Book b(store, 2);

    for (uint64_t i = 0; i < 50; ++i) a.add(100 + i, 'B', 1000000 - static_cast<int32_t>(i) * 100, 10);
    for (uint64_t i = 0; i < 30; ++i) b.add(200 + i, 'S', 2000000 + static_cast<int32_t>(i) * 100, 20);

    CHECK_EQ(a.resting_orders(), 50u);
    CHECK_EQ(b.resting_orders(), 30u);
    CHECK_EQ(store.pool.live(), 80u);

    a.execute(100, 10);          // empties the order
    a.remove(101);
    b.execute(200, 5);           // partial: the order stays

    CHECK_EQ(a.resting_orders(), 48u);
    CHECK_EQ(b.resting_orders(), 30u);
    const itchbook::book::Order* b200 = b.find(200);
    CHECK(b200 != nullptr);
    if (b200 != nullptr) CHECK_EQ(b200->shares, 15u);
    CHECK_EQ(store.pool.live(), 78u);

    // Volume is per book, not per storage. Two symbols sharing a pool must not
    // share a tape.
    CHECK_EQ(a.volume(), 10u);
    CHECK_EQ(b.volume(), 5u);

    CHECK_EQ(a.locate(), 1u);
    CHECK_EQ(b.locate(), 2u);
}

void test_a_message_about_the_pre_gap_world_is_expected_not_an_error() {
    // Every delete or execution referencing an order we threw away is a message
    // about the world before the gap. It is not corruption and it is not a bug
    // — it is the signal that recovery is not finished, and it is the only
    // signal the feed gives us.
    GapConfig c;
    c.recovery_window = 1600;
    c.recovery_tolerance = 0;
    GapTracker t{c};
    t.on_gap(1, 100);
    for (int i = 0; i < 200; ++i) t.on_reference(false);
    CHECK_EQ(t.stats().unknown_refs_after_gap, 200u);
    CHECK_EQ(t.stats().messages_untrusted, 200u);
    CHECK(!t.trusted());
    // Once the stragglers stop and a full clean window has gone by, it is over.
    for (int i = 0; i < 3200; ++i) t.on_reference(true);
    CHECK(t.trusted());
}

void test_a_stream_that_stopped_is_not_a_stream_that_ended() {
    // The bug the scenario matrix missed. A gap in the middle announces
    // itself: the next packet's sequence number has jumped. A stream that dies
    // never sends that next packet, so there is no discontinuity to see and
    // every counter stays at zero while the book misses the rest of the day.
    //
    // Found by sweeping outage lengths past the point where the feed resumes —
    // 80,235 messages, 40% of a session, gone, and the receiver called itself
    // trusted.
    GapTracker t;
    CHECK(t.trusted());
    t.on_stream_end(false);
    CHECK(!t.trusted());
    CHECK(t.halted());
    CHECK_EQ(t.stats().truncated_streams, 1u);

    // ...and an orderly end is not a failure.
    GapTracker clean;
    clean.on_stream_end(true);
    CHECK(clean.trusted());
    CHECK_EQ(clean.stats().truncated_streams, 0u);
}

void test_a_truncated_stream_outranks_a_recovery_in_progress() {
    // If the feed dies while we are still rebuilding, the answer is halted and
    // not recovering: there is no bound on what was missed and no further
    // messages with which to demonstrate convergence.
    GapConfig c;
    c.recovery_window = 10;
    c.recovery_tolerance = 0;
    GapTracker t{c};
    t.on_gap(100, 50);
    CHECK(!t.trusted());
    t.on_stream_end(false);
    CHECK(t.halted());
}

}  // namespace

int main() {
    test_a_clean_stream_is_trusted();
    test_a_gap_stops_the_book_being_trusted();
    test_halt_policy_never_rebuilds();
    test_recovery_needs_an_unbroken_run();
    test_a_second_gap_undoes_a_recovery();
    test_a_feed_that_keeps_losing_packets_is_not_a_feed_you_are_recovering_from();
    test_clearing_a_book_keeps_the_tape_and_drops_the_orders();
    test_clearing_one_book_leaves_its_neighbours_untouched();
    test_shared_storage_keeps_two_books_apart_under_ordinary_flow();
    test_one_straggler_does_not_block_recovery_forever();
    test_a_sustained_rate_of_stragglers_does_block_recovery();
    test_a_message_about_the_pre_gap_world_is_expected_not_an_error();
    test_a_stream_that_stopped_is_not_a_stream_that_ended();
    test_a_truncated_stream_outranks_a_recovery_in_progress();
    return REPORT();
}
