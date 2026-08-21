// spsc_stress — the ring under a race detector, with the interleavings varied.
//
// The unit tests in test_spsc_ring.cpp check the arithmetic and run two threads
// at full tilt, which produces one narrow band of interleavings: both sides
// saturated, the ring either always full or always empty. The races a lock-free
// structure actually has live in the other interleavings — the ring hovering
// near a boundary, one side stalling mid-batch — and the only way to reach them
// is to stall the threads at random points.
//
// TSan is what turns this from a soak into evidence. A racing store that
// happens to be benign on x86-64's strong memory model would pass this a
// billion times here and fail on a weaker one; TSan flags the race itself
// rather than waiting for it to produce a wrong answer. Which is why the build
// that runs this is a separate CI job: TSan and ASan cannot share a binary.
//
//   spsc_stress [--ops N] [--seed N]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include "itchbook/pipe/spsc_ring.hpp"

namespace {

// Wider than a word, so a torn write has somewhere to show. The sequence number
// is what every check is built on: values are their own identity, so a drop, a
// duplicate and a reorder are all the same assertion.
struct Slot {
    uint64_t seq = 0;
    uint64_t stamp = 0;
    uint64_t pad[6] = {};
};

constexpr size_t kCapacity = 1024;

// A stall the compiler cannot delete and the scheduler might act on. Sometimes
// a spin, sometimes a yield: the yield gives the other thread a real chance to
// run mid-batch, which is where the interesting interleavings are.
std::atomic<uint64_t> g_burn{0};

void stall(std::mt19937_64& rng) {
    const uint64_t r = rng();
    if ((r & 0xFF) == 0) {
        std::this_thread::yield();
        return;
    }
    const int spins = static_cast<int>(r & 0x3F);
    for (int i = 0; i < spins; ++i) g_burn.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t ops = 100000000;   // the plan's figure; CI passes something smaller
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--ops N] [--seed N]\n", argv[0]);
            return 2;
        }
    }

    itchbook::pipe::SpscRing<Slot, kCapacity> ring;
    std::atomic<uint64_t> full_events{0};

    std::thread producer([&] {
        std::mt19937_64 rng(seed);
        uint64_t next = 0;
        while (next < ops) {
            const size_t room = ring.writable();
            if (room == 0) {
                full_events.fetch_add(1, std::memory_order_relaxed);
                stall(rng);
                continue;
            }
            size_t batch = 1 + static_cast<size_t>(rng() % 32);
            if (batch > room) batch = room;
            if (batch > ops - next) batch = static_cast<size_t>(ops - next);
            for (size_t k = 0; k < batch; ++k) {
                Slot& s = ring.write_slot(k);
                s.seq = next + k;
                s.stamp = ~(next + k);
            }
            // Stall BETWEEN writing the slots and publishing them. This is the
            // window the release store exists to close: if the consumer can see
            // these slots now, the ordering is wrong and TSan should say so.
            if ((rng() & 0x3F) == 0) stall(rng);
            ring.publish(batch);
            next += batch;
            if ((rng() & 0x1F) == 0) stall(rng);
        }
    });

    std::mt19937_64 rng(seed ^ 0x9E3779B97F4A7C15ULL);
    uint64_t expect = 0;
    uint64_t out_of_order = 0;
    uint64_t torn = 0;
    uint64_t empty_events = 0;
    while (expect < ops) {
        const size_t ready = ring.readable();
        if (ready == 0) {
            ++empty_events;
            stall(rng);
            continue;
        }
        // A random prefix of what was offered, never more: readable() is a
        // lower bound, and consuming past it walks tail_ over head_.
        const size_t take = 1 + static_cast<size_t>(rng() % ready);
        for (size_t k = 0; k < take; ++k) {
            const Slot& s = ring.read_slot(k);
            if (s.seq != expect + k) ++out_of_order;
            if (s.stamp != ~(expect + k)) ++torn;
        }
        if ((rng() & 0x3F) == 0) stall(rng);
        ring.consume(take);
        expect += take;
    }
    producer.join();

    const bool ok = out_of_order == 0 && torn == 0 && expect == ops &&
                    ring.produced() == ops && ring.consumed() == ops && ring.empty();
    std::printf("ops %llu  ring full %llu  ring empty %llu  out-of-order %llu  torn %llu\n",
                static_cast<unsigned long long>(ops),
                static_cast<unsigned long long>(full_events.load()),
                static_cast<unsigned long long>(empty_events),
                static_cast<unsigned long long>(out_of_order),
                static_cast<unsigned long long>(torn));
    // A run that never reached a boundary exercised none of them and proves
    // nothing, however many operations it did.
    const bool reached_boundaries = full_events.load() > 0 && empty_events > 0;
    if (!reached_boundaries) {
        std::printf("FAIL: the ring never reached full and empty; "
                    "this run tested the easy path only\n");
    }
    std::printf("%s\n", ok ? "OK: every message exactly once, in order"
                           : "FAIL: conservation or ordering broken");
    return (ok && reached_boundaries) ? 0 : 1;
}
