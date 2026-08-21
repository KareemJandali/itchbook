// test_spsc_ring — the boundary conditions, and the ones that only appear far
// from the start.
//
// A ring passes a hundred pushes and pops without exercising anything that
// matters. What matters is the seams: the moment it fills, the moment the
// masked index wraps past the array, and the moment the counter itself has
// moved far enough that a signed/unsigned confusion or an off-by-one in the
// mask would finally show. Those are the tests here.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "itchbook/pipe/spsc_ring.hpp"
#include "tests/check.hpp"

using itchbook::pipe::SpscRing;

namespace {

void test_empty_and_full_are_distinguishable() {
    // With WRAPPED indices, head == tail means both empty and full, and the
    // usual fix is to waste a slot. Counting indices instead means a ring of 4
    // holds 4, and this asserts that it really does.
    SpscRing<uint64_t, 4> r;
    CHECK(r.empty());
    CHECK(!r.full());
    CHECK_EQ(r.size(), 0u);

    for (uint64_t i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(r.full());
    CHECK(!r.empty());
    CHECK_EQ(r.size(), 4u);

    CHECK(!r.push(99));            // refuses; does not overwrite
    CHECK_EQ(r.size(), 4u);

    uint64_t v = 0;
    for (uint64_t i = 0; i < 4; ++i) {
        CHECK(r.pop(&v));
        CHECK_EQ(v, i);            // and it is a FIFO, not a stack
    }
    CHECK(r.empty());
    CHECK(!r.pop(&v));
}

void test_fifo_order_survives_the_mask_wrapping() {
    // The array is 8 slots; push 100,000 through it one at a time. Every
    // element crosses the mask boundary 12,500 times, and an off-by-one in the
    // masking would put a value in the wrong slot long before the end.
    SpscRing<uint64_t, 8> r;
    uint64_t out = 0;
    for (uint64_t i = 0; i < 100000; ++i) {
        CHECK(r.push(i));
        uint64_t v = 0;
        CHECK(r.pop(&v));
        CHECK_EQ(v, i);
        ++out;
    }
    CHECK_EQ(out, 100000u);
    CHECK(r.empty());
    CHECK_EQ(r.produced(), r.consumed());
}

void test_partial_drain_keeps_the_order_across_a_wrap() {
    // Harder than the above: keep the ring PARTIALLY full so the read and write
    // cursors are at different offsets when the wrap happens. A mask bug that
    // cancels out when head and tail move together shows up here.
    SpscRing<uint64_t, 8> r;
    uint64_t next_push = 0;
    uint64_t next_pop = 0;
    for (int round = 0; round < 5000; ++round) {
        while (r.size() < 5) CHECK(r.push(next_push++));
        for (int i = 0; i < 3; ++i) {
            uint64_t v = 0;
            CHECK(r.pop(&v));
            CHECK_EQ(v, next_pop++);
        }
    }
    CHECK_EQ(r.size(), static_cast<size_t>(next_push - next_pop));
}

void test_batched_publication_is_all_or_nothing() {
    // The producer fills several slots and publishes once. Until publish() the
    // consumer must see NOTHING -- that is the whole point of the release
    // store, and a consumer that can see a half-filled batch is the bug the
    // memory ordering exists to prevent.
    SpscRing<uint64_t, 16> r;
    CHECK_EQ(r.readable(), 0u);
    CHECK(r.writable() >= 4);

    for (size_t i = 0; i < 4; ++i) r.write_slot(i) = 100 + i;
    CHECK_EQ(r.readable(), 0u);        // written, not published

    r.publish(4);
    CHECK_EQ(r.readable(), 4u);
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(r.read_slot(i), 100 + i);
    r.consume(4);
    CHECK_EQ(r.readable(), 0u);
    CHECK(r.empty());
}

void test_writable_and_readable_are_lower_bounds_never_overstatements() {
    // The first version of this test asserted equality with size() and failed,
    // which was the test being wrong about the contract rather than the ring
    // being wrong about the count -- but the failure was worth having, because
    // the contract is not what the names suggest.
    //
    // Both accessors refresh their view of the OTHER thread only when they
    // would otherwise report zero. That is the entire point: it turns a
    // cross-core read per message into one per batch. The consequence is that
    // between refreshes they under-report, and the property that has to hold is
    // therefore one-sided.
    SpscRing<uint64_t, 16> r;
    for (size_t i = 0; i < 16; ++i) {
        CHECK(r.writable() <= 16 - i);      // never claims room that is not there
        CHECK(r.readable() <= i);           // never offers data that is not published
        CHECK(r.push(i));
    }
    // Exhausted: zero is the one answer that is never stale, because reporting
    // it forces the refresh.
    CHECK_EQ(r.writable(), 0u);
    CHECK(r.full());

    // size() is the unstale view, and it is what a monitor should read. The
    // difference between it and readable() is the staleness, and asserting the
    // direction of that difference is the contract.
    CHECK_EQ(r.size(), 16u);
    CHECK(r.readable() <= r.size());

    // Draining is done through what readable() OFFERS, never through size().
    // Consuming more than was offered walks tail_ past head_, and because the
    // counts are unsigned the next readable() reports billions rather than a
    // negative number. The first version of this test did exactly that.
    while (r.size() > 0) {
        const size_t n = r.readable();
        CHECK(n > 0);
        r.consume(n);
    }
    CHECK(r.empty());
    CHECK_EQ(r.readable(), 0u);
    CHECK_EQ(r.writable(), 16u);
}

void test_a_stale_view_costs_a_batch_and_never_a_message() {
    // The under-reporting is only acceptable because draining what you are told
    // and asking again converges. If it did not, a consumer could starve while
    // the ring held data.
    SpscRing<uint64_t, 16> r;
    for (uint64_t i = 0; i < 16; ++i) CHECK(r.push(i));

    uint64_t seen = 0;
    size_t asks = 0;
    while (seen < 16) {
        const size_t n = r.readable();
        CHECK(n > 0);                       // never zero while data is published
        ++asks;
        for (size_t k = 0; k < n; ++k) CHECK_EQ(r.read_slot(k), seen + k);
        r.consume(n);
        seen += n;
    }
    CHECK_EQ(seen, 16u);
    CHECK(asks <= 16);                      // converges; does not go one at a time forever
}

void test_the_counter_keeps_counting_past_the_array() {
    // produced() and consumed() are monotonic counts, not positions. After
    // 1,000 trips round an 8-slot ring they read 8,000, and anything that
    // wrapped them to a position would read 0.
    SpscRing<uint64_t, 8> r;
    for (uint64_t i = 0; i < 8000; ++i) {
        CHECK(r.push(i));
        uint64_t v = 0;
        CHECK(r.pop(&v));
    }
    CHECK_EQ(r.produced(), 8000u);
    CHECK_EQ(r.consumed(), 8000u);
    CHECK_EQ(r.size(), 0u);
}

void test_two_threads_conserve_every_message_and_their_order() {
    // The single-threaded tests above check the arithmetic. This checks the
    // thing the arithmetic is for: with a producer and a consumer actually
    // running at once, on a ring small enough to be full and empty constantly,
    // every message arrives exactly once and in order.
    //
    // The values are their own sequence numbers, so a duplicate, a drop or a
    // reorder are all the same assertion. Under TSan this is also the race
    // detector's opportunity -- see the CI job.
    constexpr uint64_t kMessages = 2000000;
    SpscRing<uint64_t, 64> ring;

    std::thread producer([&ring] {
        for (uint64_t i = 0; i < kMessages;) {
            const size_t room = ring.writable();
            if (room == 0) continue;                 // full: spin, drop nothing
            const size_t batch = room < 16 ? room : 16;
            size_t n = 0;
            for (; n < batch && i + n < kMessages; ++n) ring.write_slot(n) = i + n;
            ring.publish(n);
            i += n;
        }
    });

    uint64_t expected = 0;
    uint64_t wrong = 0;
    while (expected < kMessages) {
        const size_t ready = ring.readable();
        if (ready == 0) continue;
        for (size_t n = 0; n < ready; ++n) {
            if (ring.read_slot(n) != expected + n) ++wrong;
        }
        ring.consume(ready);
        expected += ready;
    }
    producer.join();

    CHECK_EQ(wrong, 0u);
    CHECK_EQ(expected, kMessages);
    CHECK_EQ(ring.produced(), kMessages);
    CHECK_EQ(ring.consumed(), kMessages);
    CHECK(ring.empty());
}

void test_a_slow_consumer_never_loses_a_message() {
    // The producer is deliberately faster than the consumer, so the ring is
    // full most of the time and writable() keeps returning zero. Nothing may be
    // lost or duplicated: a full ring is a REFUSAL, and what to do about it is
    // the caller's policy, not the ring's.
    constexpr uint64_t kMessages = 200000;
    SpscRing<uint64_t, 8> ring;
    uint64_t sum_in = 0;

    std::thread producer([&ring, &sum_in] {
        for (uint64_t i = 1; i <= kMessages;) {
            if (ring.push(i)) { sum_in += i; ++i; }
        }
    });

    std::atomic<uint64_t> drag{0};
    uint64_t sum_out = 0;
    uint64_t got = 0;
    while (got < kMessages) {
        uint64_t v = 0;
        if (ring.pop(&v)) {
            sum_out += v;
            ++got;
            // Make the consumer the slow side on purpose. An atomic rather
            // than a volatile int: incrementing a volatile is deprecated in
            // C++20, and this only has to be work the compiler cannot delete.
            for (int spin = 0; spin < 8; ++spin) {
                drag.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    producer.join();

    CHECK_EQ(got, kMessages);
    CHECK_EQ(sum_out, kMessages * (kMessages + 1) / 2);
    CHECK_EQ(sum_out, sum_in);
    CHECK(ring.empty());
    CHECK(drag.load() > 0);   // the spin was not optimised away
}

// The bug that phase 10.6 found four layers up, reduced to the ring.
//
// A producer that needs the TRUE free count -- because it has to decide
// something irreversible, like whether to drop a packet -- cannot use
// writable(), which is a lower bound. The obvious substitute is
// `capacity() - size()`, which returns the right number and silently breaks the
// ring: writable() keeps a stale copy of the consumer's cursor and refreshes it
// only when it would report zero, which is sound only while the producer
// publishes no more than writable() itself offered. Publish against size() and
// `head - cached_tail_` walks past Capacity, so the next writable() underflows
// in unsigned arithmetic and reports about eighteen quintillion free slots.
//
// Nothing is lost when that happens, which is why it survived every count-based
// check: the producer overwrites slots the consumer has not read, and the
// messages come out REORDERED by exactly one lap of the ring.
void test_the_true_free_count_does_not_corrupt_the_cached_view() {
    SpscRing<uint64_t, 8> r;
    uint64_t v = 0;

    // Drive the producer's cached view of the consumer as stale as it can be:
    // fill, drain completely, and never let writable() hit zero and refresh.
    for (uint64_t i = 0; i < 4; ++i) CHECK(r.push(i));
    for (uint64_t i = 0; i < 4; ++i) CHECK(r.pop(&v));
    CHECK(r.empty());

    // writable() may now under-report -- that is its contract and not a bug.
    // writable_exact() must report the truth AND leave the cache consistent.
    CHECK(r.writable() <= 8);
    CHECK_EQ(r.writable_exact(), size_t{8});

    // The invariant the whole structure rests on: after any number of
    // writable_exact() calls, publishing what it offered must never make
    // writable() overstate. Ten laps, filling to whatever the exact count says
    // each time, is enough for a broken cache to run away.
    for (int lap = 0; lap < 10; ++lap) {
        const size_t room = r.writable_exact();
        CHECK(room <= 8);
        for (size_t k = 0; k < room; ++k) r.write_slot(k) = static_cast<uint64_t>(lap * 100 + k);
        r.publish(room);
        CHECK(r.writable() <= 8);          // never eighteen quintillion
        CHECK_EQ(r.size(), room);
        for (size_t k = 0; k < room; ++k) {
            CHECK(r.pop(&v));
            CHECK_EQ(v, static_cast<uint64_t>(lap * 100 + static_cast<int>(k)));
        }
        CHECK(r.empty());
    }

    // And it never overstates: whatever it offers, the ring can actually hold.
    for (uint64_t i = 0; i < 5; ++i) CHECK(r.push(100 + i));
    CHECK_EQ(r.writable_exact(), size_t{3});
    CHECK_EQ(r.writable_exact() + r.size(), size_t{8});
}

}  // namespace

int main() {
    test_empty_and_full_are_distinguishable();
    test_fifo_order_survives_the_mask_wrapping();
    test_partial_drain_keeps_the_order_across_a_wrap();
    test_batched_publication_is_all_or_nothing();
    test_writable_and_readable_are_lower_bounds_never_overstatements();
    test_a_stale_view_costs_a_batch_and_never_a_message();
    test_the_counter_keeps_counting_past_the_array();
    test_two_threads_conserve_every_message_and_their_order();
    test_a_slow_consumer_never_loses_a_message();
    test_the_true_free_count_does_not_corrupt_the_cached_view();
    return REPORT();
}
