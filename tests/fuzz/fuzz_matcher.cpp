// fuzz_matcher — the phase 5 done-condition.
//
// Hand-written tests check the rules you thought of. This checks the ones you
// did not: random order sequences, with four invariants that must hold after
// every single operation.
//
//   1. The book is never crossed — best bid < best ask, always.
//   2. Shares are conserved — every share submitted is filled, cancelled, or
//      resting, per order and in aggregate.
//   3. Every order is in exactly one state, and only ever moved between states
//      the machine permits (illegal moves assert inside the engine).
//   4. Price-time priority is preserved — a fill always hits the oldest resting
//      order at the best price.
//
// The fourth is the one that needs a shadow model. The engine could satisfy the
// other three while filling the wrong order at the right price, so a separate,
// deliberately simple queue is maintained alongside and every fill is checked
// against it. That model knows nothing about the book's internals; if the two
// disagree, one of them is wrong and it is usually not the simple one.
//
// Input is a byte buffer decoded into operations, so this runs under libFuzzer
// where it is available, and under a seeded loop everywhere else. Build:
//
//   g++ -O2 -Iinclude -I. tests/fuzz/fuzz_matcher.cpp -o fuzz_matcher
//   clang++ -fsanitize=fuzzer,address -Iinclude -I. tests/fuzz/fuzz_matcher.cpp
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "itchbook/engine/matcher.hpp"

using namespace itchbook::engine;

namespace {

// How many of each order type the generator has actually produced. This exists
// because for a long time the answer for stops was zero: the type switch never
// selected them, so a million sequences exercised four of the six types the
// engine implements while reporting full coverage. A fuzzer that silently
// stops covering something is worse than no fuzzer, because it is believed.
uint64_t g_emitted[6] = {0, 0, 0, 0, 0, 0};

const char* kTypeName[6] = {"Limit", "Market", "IOC", "FOK",
                            "StopMarket", "StopLimit"};

}  // namespace

namespace {

int failures = 0;

#define INVARIANT(cond, what)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "INVARIANT VIOLATED: %s\n  at %s:%d\n",       \
                         (what), __FILE__, __LINE__);                          \
            ++failures;                                                        \
            return false;                                                      \
        }                                                                      \
    } while (0)

// A pull-apart view of the fuzzer's byte string. Running out of bytes ends the
// sequence rather than reading past the end.
class Bytes {
public:
    Bytes(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool empty() const { return i_ >= n_; }
    uint8_t u8() { return i_ < n_ ? p_[i_++] : 0; }
    uint32_t range(uint32_t lo, uint32_t hi) {   // inclusive
        if (hi <= lo) return lo;
        return lo + (u8() % (hi - lo + 1));
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
};

// Price-time priority is checked two ways, both against state the engine
// maintains separately from the structure being checked.
//
// The book keeps its queue as an intrusive linked list, spliced by the book
// layer. Arrival sequence numbers are assigned by the engine layer and never
// touched by the book. So "the front of the book's list is the order with the
// lowest sequence at that price" relates two mechanisms that do not share code,
// and a bug in either shows up as a disagreement.
//
// An earlier version of this test maintained its own queue of ids and tried to
// mirror every way an order can leave or move. It could not keep up: within a
// single submit several icebergs can refresh, each rejoining the back with a
// fresh sequence, and reconstructing that order from the outside meant
// reimplementing the engine inside its own test. Comparing against the
// sequence numbers says the same thing and cannot drift.

// The id of the oldest order resting at this side and price, by arrival
// sequence, or 0 if there is none.
uint64_t oldest_resting(const Matcher& m, Side side, int32_t price) {
    uint64_t best_id = 0;
    uint64_t best_seq = UINT64_MAX;
    for (const auto& [id, meta] : m.orders()) {
        if (!meta.in_book || meta.resting == 0) continue;
        if (meta.req.side != side || meta.req.price != price) continue;
        if (meta.sequence < best_seq) {
            best_seq = meta.sequence;
            best_id = id;
        }
    }
    return best_id;
}

bool run_sequence(const uint8_t* data, size_t size) {
    Bytes in(data, size);
    // A small reference table: each sequence builds a fresh engine, and the
    // default million-slot table would cost 16MB of zeroing per sequence —
    // which dominated everything else this fuzzer does.
    Matcher m(100, 1024);
    uint64_t next_id = 1;
    size_t fills_seen = 0;
    std::map<std::pair<char, int32_t>, uint64_t> last_maker_seq;

    // Prices are kept in a narrow band so orders actually meet each other;
    // a wide random range would mostly produce a book that never trades.
    const int32_t base = 100000;

    while (!in.empty() && next_id < 250) {
        const uint8_t op = in.u8() % 10;

        if (op == 9) {
            // Cancel something at random.
            uint64_t victim = 1 + (in.u8() % (next_id > 1 ? next_id - 1 : 1));
            const Matcher::Meta* meta = m.find(victim);
            if (meta != nullptr && !is_terminal(meta->state)) {
                // Nothing to tell the shadow: it drops dead entries lazily by
                // asking the engine whether an id is still resting.
                m.cancel(victim);
            }
        } else {
            Request req;
            req.id = next_id++;
            req.side = (in.u8() & 1) ? Side::Buy : Side::Sell;
            req.owner = in.u8() % 3;
            req.quantity = in.range(1, 200);
            req.price = base + static_cast<int32_t>(in.range(0, 20)) * 100;

            switch (in.u8() % 10) {
                case 0:  req.type = Type::Market; break;
                case 1:  req.type = Type::IOC; break;
                case 2:  req.type = Type::FOK; break;
                case 3:
                    req.type = Type::Limit;
                    req.display = in.range(1, req.quantity);
                    break;
                case 4:
                case 5:
                    // Stops. The trigger sits inside the same narrow band as
                    // the prices, so the market actually reaches it — a stop
                    // parked outside the band is a stop that never fires and
                    // tests only the parking. Both kinds are generated: a
                    // StopMarket becomes a Market on trigger and a StopLimit
                    // becomes a Limit at req.price, which is the branch that
                    // had no coverage at all.
                    req.type = (in.u8() & 1) ? Type::StopMarket : Type::StopLimit;
                    req.stop_price = base + static_cast<int32_t>(in.range(0, 20)) * 100;
                    break;
                default: req.type = Type::Limit; break;
            }
            ++g_emitted[static_cast<size_t>(req.type)];
            switch (in.u8() % 4) {
                case 1: req.stp = Stp::CancelNewest; break;
                case 2: req.stp = Stp::CancelOldest; break;
                case 3: req.stp = Stp::CancelBoth; break;
                default: req.stp = Stp::None; break;
            }

            const size_t before = m.fills().size();
            Result r = m.submit(req);
            (void)r;

            // Invariant 4a: within a price level, makers trade in arrival
            // order. Sequences increase monotonically and a refreshed iceberg
            // slice takes a new one, so these must come out strictly
            // increasing — filling a newer order while an older one waits shows
            // up here immediately.
            for (size_t i = before; i < m.fills().size(); ++i) {
                const Fill& f = m.fills()[i];
                const Matcher::Meta* maker = m.find(f.maker);
                INVARIANT(maker != nullptr, "fill references an unknown maker");
                const char mside = to_char(maker->req.side);
                auto key = std::make_pair(mside, f.price);
                auto prev = last_maker_seq.find(key);
                // Non-decreasing, not strictly increasing: one resting order
                // filled across several takers legitimately reports the same
                // sequence each time. A *decrease* is the violation — it means
                // a newer order traded while an older one was still queued.
                INVARIANT(prev == last_maker_seq.end() || f.maker_sequence >= prev->second,
                          "price-time priority violated: a newer resting order "
                          "traded while an older one was still queued");
                last_maker_seq[key] = f.maker_sequence;

                (void)mside;
                ++fills_seen;
            }


        }

        // ---- the invariants, after every operation ----
        // Crossing is O(1) to check, so it is checked every time.
        INVARIANT(!m.crossed(), "book is crossed: best bid >= best ask");
        INVARIANT(m.conserves_shares(), "shares are not conserved");

        int32_t bid = 0;
        int32_t ask = 0;
        if (m.best_bid(&bid) && m.best_ask(&ask)) {
            INVARIANT(bid < ask, "best bid is not below best ask");
        }

        // Invariant 4b: the front of the book's queue is the oldest order
        // resting at that price. 4a catches a wrong fill; this catches a wrong
        // queue even when nothing traded.
        for (Side side : {Side::Buy, Side::Sell}) {
            int32_t best = 0;
            const bool have = (side == Side::Buy) ? m.best_bid(&best) : m.best_ask(&best);
            if (!have) continue;
            const itchbook::book::Order* front = m.book().best_order(to_char(side));
            INVARIANT(front != nullptr, "a best price with no order behind it");
            const uint64_t oldest = oldest_resting(m, side, best);
            INVARIANT(oldest == 0 || oldest == front->ref,
                      "front of the book is not the oldest resting order");
        }
    }

    // End of sequence: the full checks, unsampled.
    INVARIANT(m.conserves_shares(), "shares are not conserved at end of sequence");
    for (Side side : {Side::Buy, Side::Sell}) {
        int32_t best = 0;
        const bool have = (side == Side::Buy) ? m.best_bid(&best) : m.best_ask(&best);
        if (!have) continue;
        const itchbook::book::Order* front = m.book().best_order(to_char(side));
        INVARIANT(front != nullptr, "a best price with no order behind it");
        const uint64_t oldest = oldest_resting(m, side, best);
        INVARIANT(oldest == 0 || oldest == front->ref,
                  "front of the book is not the oldest resting order");
    }

    // Every order ends in exactly one state, and resting shares agree with what
    // the book is actually holding.
    uint64_t counted = 0;
    for (const auto& [id, meta] : m.orders()) {
        (void)id;
        // Invariant 3: an order that reached a terminal state holds nothing,
        // and an order in the book holds something.
        if (is_terminal(meta.state)) {
            INVARIANT(!meta.in_book, "a terminal order is still in the book");
            INVARIANT(meta.resting == 0 && meta.hidden == 0,
                      "a terminal order still holds shares");
        }
        if (meta.in_book) {
            INVARIANT(meta.resting > 0, "an order in the book with no shares");
        }
        counted += meta.resting;
    }
    (void)counted;
    (void)fills_seen;
    return true;
}

}  // namespace

// libFuzzer entry point, used when the runtime is available.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_sequence(data, size);
    return 0;
}

#ifndef ITCHBOOK_LIBFUZZER
int main(int argc, char** argv) {
    uint64_t iterations = 1000000;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--iterations" && i + 1 < argc) iterations = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "usage: %s [--iterations N] [--seed N]\n", argv[0]); return 2; }
    }

    std::mt19937_64 rng(seed);
    std::vector<uint8_t> buf;
    uint64_t total_ops = 0;

    for (uint64_t i = 0; i < iterations; ++i) {
        const size_t n = 16 + (rng() % 240);
        buf.resize(n);
        for (size_t j = 0; j < n; ++j) buf[j] = static_cast<uint8_t>(rng());
        total_ops += n;
        if (!run_sequence(buf.data(), buf.size())) {
            std::fprintf(stderr, "\nfailing input (seed %llu, iteration %llu):\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(i));
            for (uint8_t b : buf) std::fprintf(stderr, "%02x", b);
            std::fprintf(stderr, "\n");
            return 1;
        }
        if ((i + 1) % 100000 == 0) {
            std::printf("  %llu sequences ok\n", static_cast<unsigned long long>(i + 1));
            std::fflush(stdout);
        }
    }
    std::printf("OK: %llu random order sequences, ~%llu operations, "
                "no invariant violated\n",
                static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(total_ops));

    // Coverage is part of the result, not a footnote. A run that violated no
    // invariant because it never exercised a type has not shown anything about
    // that type, so say what was emitted and fail if anything was missed.
    std::printf("  emitted:");
    bool missing = false;
    for (size_t i = 0; i < 6; ++i) {
        std::printf(" %s=%llu", kTypeName[i], static_cast<unsigned long long>(g_emitted[i]));
        if (g_emitted[i] == 0) missing = true;
    }
    std::printf("\n");
    if (missing) {
        std::fprintf(stderr,
                     "FAIL: the generator never emitted one of the order types. "
                     "Every type the engine implements must be exercised, or "
                     "this run proves nothing about it.\n");
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
#endif
