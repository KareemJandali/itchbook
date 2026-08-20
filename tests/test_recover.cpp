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
    // before the gap is still out there, so the count restarts — "mostly
    // clean" is not a state the book can be trusted in.
    GapConfig c;
    c.clean_messages_to_recover = 100;
    GapTracker t{c};
    t.on_gap(1, 10);

    for (int i = 0; i < 99; ++i) t.on_reference(true);
    CHECK(!t.trusted());
    t.on_reference(false);                    // one straggler resets it
    CHECK(!t.trusted());
    CHECK_EQ(t.clean_run(), 0u);

    for (int i = 0; i < 99; ++i) t.on_reference(true);
    CHECK(!t.trusted());
    t.on_reference(true);                     // the hundredth
    CHECK(t.trusted());
    CHECK_EQ(t.stats().recoveries, 1u);
    CHECK_EQ(t.stats().unknown_refs_after_gap, 1u);
}

void test_a_second_gap_undoes_a_recovery() {
    GapConfig c;
    c.clean_messages_to_recover = 10;
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
    c.clean_messages_to_recover = 1;
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

void test_a_message_about_the_pre_gap_world_is_expected_not_an_error() {
    // Every delete or execution referencing an order we threw away is a message
    // about the world before the gap. It is not corruption and it is not a bug
    // — it is the signal that recovery is not finished, and it is the only
    // signal the feed gives us.
    GapConfig c;
    c.clean_messages_to_recover = 5;
    GapTracker t{c};
    t.on_gap(1, 100);
    for (int i = 0; i < 20; ++i) t.on_reference(false);
    CHECK_EQ(t.stats().unknown_refs_after_gap, 20u);
    CHECK_EQ(t.stats().messages_untrusted, 20u);
    CHECK(!t.trusted());
    for (int i = 0; i < 5; ++i) t.on_reference(true);
    CHECK(t.trusted());
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
    test_a_message_about_the_pre_gap_world_is_expected_not_an_error();
    return REPORT();
}
