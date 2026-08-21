// test_live_orders — the counting structure behind itch_census --peak-orders.
//
// This exists because the structure got a number wrong the first time it met a
// real file, and its own self-check is what caught it. The tests below are the
// ones that would have caught it earlier: every one of them forces the table to
// grow, because growth is where it went wrong and 200-order unit tests never
// reach it.
#include <cstdint>

#include "itchbook/itch/live_orders.hpp"
#include "tests/check.hpp"

using itchbook::itch::LiveOrders;

namespace {

void test_the_identity_holds_across_a_rehash() {
    // The bug: grow() rehashed by calling insert(), so every slot it moved was
    // counted as a new order arriving. On the real day that was 1,572,864
    // arrivals that never happened. Start small so a few thousand orders force
    // several doublings.
    LiveOrders live(16);
    const uint64_t n = 5000;
    for (uint64_t i = 0; i < n; ++i) live.insert(i, 100);

    CHECK(live.rehashes() > 0);            // the test is worthless without this
    CHECK_EQ(live.inserts(), n);           // ...and this is what the bug broke
    CHECK_EQ(live.size(), n);
    CHECK_EQ(live.peak(), n);
    CHECK(live.accounts());

    // Take half out by deleting and half by executing to zero, so both exit
    // doors are exercised against a table that has been rehashed.
    for (uint64_t i = 0; i < n; i += 2) live.erase(i);
    for (uint64_t i = 1; i < n; i += 2) CHECK(live.reduce(i, 100));

    CHECK_EQ(live.size(), 0u);
    CHECK_EQ(live.removed(), n / 2);
    CHECK_EQ(live.emptied(), n / 2);
    CHECK_EQ(live.peak(), n);              // the high-water mark is not undone
    CHECK(live.accounts());
}

void test_a_rehash_does_not_move_the_peak() {
    // The peak was in fact correct through the bug, because grow() resets the
    // count and walks it straight back up. Asserted rather than assumed: it is
    // the number the shared reference map gets sized from, and "it happened to
    // be right" is not a property.
    LiveOrders live(16);
    for (uint64_t i = 0; i < 1000; ++i) live.insert(i, 10);
    const size_t peak_before = live.peak();
    CHECK_EQ(peak_before, 1000u);

    for (uint64_t i = 0; i < 900; ++i) live.erase(i);
    for (uint64_t i = 1000; i < 3000; ++i) live.insert(i, 10);   // forces more growth

    CHECK(live.rehashes() > 1);
    CHECK_EQ(live.peak(), 2100u);          // 100 survivors + 2000 new
    CHECK_EQ(live.size(), 2100u);
    CHECK(live.accounts());
}

void test_a_partial_execution_is_not_a_departure() {
    LiveOrders live(16);
    live.insert(1, 500);
    CHECK(!live.reduce(1, 100));           // 400 left
    CHECK(!live.reduce(1, 300));           // 100 left
    CHECK_EQ(live.size(), 1u);
    CHECK_EQ(live.emptied(), 0u);
    CHECK(live.reduce(1, 100));            // gone
    CHECK_EQ(live.size(), 0u);
    CHECK_EQ(live.emptied(), 1u);
    CHECK(live.accounts());

    // An execution larger than the resting size must not wrap the unsigned
    // count, which is the same trap Book::reduce documents.
    live.insert(2, 50);
    CHECK(live.reduce(2, 4000000000u));
    CHECK_EQ(live.size(), 0u);
    CHECK(live.accounts());
}

void test_a_reference_that_names_nothing_is_counted_not_guessed() {
    LiveOrders live(16);
    live.insert(1, 100);
    CHECK(!live.erase(99));
    CHECK(!live.reduce(99, 10));
    CHECK_EQ(live.unknown(), 2u);
    CHECK_EQ(live.size(), 1u);
    CHECK(live.accounts());                // a miss changes nothing it accounts for
}

void test_a_duplicate_reference_overwrites_and_says_so() {
    // ITCH references are unique across a day's feed, so this should never fire
    // on real data -- which is exactly why it is counted separately. A feed
    // where it does fire is a feed whose claim of uniqueness is false.
    LiveOrders live(16);
    live.insert(7, 100);
    live.insert(7, 250);
    CHECK_EQ(live.size(), 1u);
    CHECK_EQ(live.inserts(), 1u);
    CHECK_EQ(live.duplicates(), 1u);
    CHECK(live.reduce(7, 250));            // the second write is what is there
    CHECK(live.accounts());
}

void test_backward_shift_keeps_a_collision_chain_reachable() {
    // Every reference here lands in the same starting slot, so they form one
    // long probe chain. Erasing from the middle of it is where a tombstone
    // implementation would leak and a naive shift would lose an entry.
    const size_t cap = 1024;
    LiveOrders live(cap);
    for (uint64_t i = 0; i < 100; ++i) live.insert(i * cap, 100 + i);

    for (uint64_t i = 0; i < 100; i += 3) CHECK(live.erase(i * cap));

    // Everything not erased must still be findable, and everything erased must
    // not be. reduce() by one share is the probe: it locates the order without
    // removing it, and misses land in unknown(), so the counter says which
    // happened without needing an accessor that only a test would use.
    size_t expected_misses = 0;
    for (uint64_t i = 0; i < 100; ++i) {
        const uint64_t before = live.unknown();
        live.reduce(i * cap, 1);
        const bool missed = live.unknown() > before;
        if (i % 3 == 0) {
            CHECK(missed);          // erased, and the shift did not resurrect it
            ++expected_misses;
        } else {
            CHECK(!missed);         // survived, and the shift did not lose it
        }
    }
    CHECK_EQ(live.unknown(), expected_misses);
    CHECK(live.accounts());
}

}  // namespace

int main() {
    test_the_identity_holds_across_a_rehash();
    test_a_rehash_does_not_move_the_peak();
    test_a_partial_execution_is_not_a_departure();
    test_a_reference_that_names_nothing_is_counted_not_guessed();
    test_a_duplicate_reference_overwrites_and_says_so();
    test_backward_shift_keeps_a_collision_chain_reachable();
    return REPORT();
}
