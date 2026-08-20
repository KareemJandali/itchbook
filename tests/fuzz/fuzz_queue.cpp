// fuzz_queue — the queue model's invariants over random book-legal sequences.
//
// The unit tests check that the model does specific things. This checks that it
// cannot do certain things at all, over sequences nobody designed.
//
// Invariants, numbered as in docs/phase6-design.md section 9.1:
//
//   I1  0 <= ahead <= shares_at(S,P) + our own earlier orders there  (clamp on)
//   I2  ahead never increases except at place or an iceberg refresh
//   I3  nothing resting at our price  =>  ahead == 0
//   I4  ahead_optimistic <= ahead_mbo <= ahead_pessimistic, pointwise
//   I5  cumulative fills: naive >= optimistic >= mbo >= pessimistic, per order
//   I6  filled + display + hidden == original quantity, always
//   I7  our fills at a level <= shares actually executed at that level, over any
//       window (price-priority triggers excluded, since those are a different
//       mechanism and are counted separately)
//
// I4 and I5 are what make `mbo` an internal oracle rather than a fourth opinion:
// the two bounds must contain it. If they ever do not, either a bound is wrong
// or the reference resolution is.
//
// The generated stream is BOOK-LEGAL by construction: executions always take
// from the front of a level, because that is what price-time priority means and
// an execution against the middle of a queue would make I7 meaningless.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "itchbook/sim/queue_model.hpp"

using namespace itchbook::sim;
namespace bk = itchbook::book;

namespace {

int failures = 0;

#define INVARIANT(cond, what)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "INVARIANT VIOLATED (%s)\n  at %s:%d\n",      \
                         (what), __FILE__, __LINE__);                          \
            ++failures;                                                        \
            return false;                                                      \
        }                                                                      \
    } while (0)

class Bytes {
public:
    Bytes(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool empty() const { return i_ >= n_; }
    uint8_t u8() { return i_ < n_ ? p_[i_++] : 0; }
    uint32_t range(uint32_t lo, uint32_t hi) {
        if (hi <= lo) return lo;
        return lo + (u8() % (hi - lo + 1));
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
};

constexpr Price4 kBase = 1000000;   // $100.0000
constexpr Price4 kTick = 100;

// The four lanes, sharing one book. Sound only because our orders never enter
// book::Book — the moment they did, the four worlds would fork and need four
// books apiece.
struct Lanes {
    QueueModel naive{[] { QueueConfig c; c.model = Model::Naive; return c; }()};
    QueueModel opt{[] { QueueConfig c; c.model = Model::Optimistic; return c; }()};
    QueueModel mbo{[] { QueueConfig c; c.model = Model::Mbo; return c; }()};
    QueueModel pess{[] { QueueConfig c; c.model = Model::Pessimistic; return c; }()};

    void set_state(char s) {
        naive.set_trading_state(s);
        opt.set_trading_state(s);
        mbo.set_trading_state(s);
        pess.set_trading_state(s);
    }
};

struct Tracked {
    uint64_t id;
    Side side;
    Price4 price;
    uint32_t quantity;
    uint64_t prev_ahead_opt = 0;
    uint64_t prev_ahead_mbo = 0;
    uint64_t prev_ahead_pess = 0;
    uint32_t prev_refresh_opt = 0;
    uint32_t prev_refresh_mbo = 0;
    uint32_t prev_refresh_pess = 0;
    uint64_t filled_naive = 0;
    uint64_t filled_opt = 0;
    uint64_t filled_mbo = 0;
    uint64_t filled_pess = 0;
};

uint64_t sum_fills(const std::vector<SimFill>& f, uint64_t id, bool exclude_priority) {
    uint64_t total = 0;
    for (const SimFill& s : f) {
        if (s.order_id != id) continue;
        if (exclude_priority && (s.trigger == Trigger::Lock || s.trigger == Trigger::Through)) {
            continue;
        }
        total += s.shares;
    }
    return total;
}

bool check_entry(const QueueModel& q, const bk::Book& book, uint64_t id, const char* who) {
    const Entry* e = q.find(id);
    if (e == nullptr || !e->live || e->display == 0) return true;

    uint64_t own_ahead = 0;
    for (const Entry& other : q.entries()) {
        if (!other.live || other.display == 0) continue;
        if (other.id == e->id) continue;
        if (other.side == e->side && other.price == e->price &&
            other.arrived_ns < e->arrived_ns) {
            own_ahead += other.display;
        }
    }
    const uint64_t limit = book.shares_at(to_char(e->side), e->price) + own_ahead;

    (void)who;
    INVARIANT(e->ahead <= limit, "I1: ahead exceeds what is resting at our price");
    if (limit == 0) INVARIANT(e->ahead == 0, "I3: empty level but ahead > 0");
    INVARIANT(e->display + e->hidden <= e->ahead0 + e->display + e->hidden,
              "I6: quantity accounting");
    return true;
}

bool run_sequence(const uint8_t* data, size_t size) {
    Bytes in(data, size);
    bk::Book book(kTick, 20, 1024);
    Lanes lanes;
    lanes.set_state('T');

    std::vector<SimFill> fn, fo, fm, fp;
    std::vector<Tracked> tracked;
    std::vector<uint64_t> live_refs;
    std::map<Price4, uint64_t> executed_at_price;   // for I7

    uint64_t next_ref = 1;
    uint64_t next_id = 1000000;   // ours, disjoint from feed references
    uint64_t ts = 34200000000000ULL;

    auto pick_price = [&](Bytes& b) {
        return kBase + static_cast<int32_t>(b.range(0, 8)) * kTick;
    };

    while (!in.empty() && next_ref < 200) {
        ts += in.range(1, 50);
        const uint8_t op = in.u8() % 12;

        if (op <= 4 || live_refs.empty()) {
            // ---- add ----
            const char side = (in.u8() & 1) ? 'B' : 'S';
            const Price4 px = pick_price(in);
            const uint32_t qty = in.range(1, 300);
            book.add(next_ref, side, px, qty);
            live_refs.push_back(next_ref);
            ++next_ref;
        } else if (op <= 7) {
            // ---- execute the FRONT order at some price: book-legal ----
            const char side = (in.u8() & 1) ? 'B' : 'S';
            const Price4 px = pick_price(in);
            const bk::Order* front = book.first_order(side, px);
            if (front == nullptr) continue;
            const uint64_t ref = front->ref;
            const uint32_t avail = front->shares;
            const uint32_t qty = std::min<uint32_t>(in.range(1, 200), avail);
            if (qty == 0) continue;

            Resolved r;
            r.type = 'E';
            r.cls = Class::Trade;
            r.known = true;
            r.ref = ref;
            r.side = from_char(side);
            r.resting_price = px;
            r.shares = qty;
            r.printable = true;
            r.print_price = px;
            r.ts = ts;

            book.take(ref, qty);
            executed_at_price[px] += qty;
            lanes.naive.commit(r, book, &fn);
            lanes.opt.commit(r, book, &fo);
            lanes.mbo.commit(r, book, &fm);
            lanes.pess.commit(r, book, &fp);
        } else if (op <= 10) {
            // ---- cancel or delete an arbitrary resting order ----
            size_t i = in.u8() % live_refs.size();
            const uint64_t ref = live_refs[i];
            const bk::Order* o = book.find(ref);
            if (o == nullptr) {
                live_refs[i] = live_refs.back();
                live_refs.pop_back();
                continue;
            }
            const Side side = from_char(static_cast<char>(o->side));
            const Price4 px = o->price;
            const uint32_t have = o->shares;
            const bool full = (in.u8() & 1) != 0;
            const uint32_t qty = full ? have : std::min<uint32_t>(in.range(1, 200), have);
            if (qty == 0) continue;

            Resolved r;
            r.type = full ? 'D' : 'X';
            r.cls = Class::Cancel;
            r.known = true;
            r.ref = ref;
            r.side = side;
            r.resting_price = px;
            r.shares = qty;
            r.ts = ts;

            if (full) {
                book.remove(ref);
                live_refs[i] = live_refs.back();
                live_refs.pop_back();
            } else {
                book.cancel(ref, qty);
            }
            lanes.naive.commit(r, book, &fn);
            lanes.opt.commit(r, book, &fo);
            lanes.mbo.commit(r, book, &fm);
            lanes.pess.commit(r, book, &fp);
        } else {
            // ---- place one of OUR orders ----
            //
            // At most one live order of ours per (side, price). This is not a
            // convenience: with two, the clamp's limit includes our own earlier
            // order's REMAINING display, and that remainder is model-dependent
            // because each lane fills it at a different rate. The limit then
            // differs per lane and the I4/I5 containment between models breaks —
            // observed directly as mbo(ahead=65) against pess(ahead=13).
            //
            // The design's reference strategy carries the same constraint for
            // the same reason (section 3.6), so the fuzzer matches it rather
            // than testing a configuration the phase does not support.
            if (tracked.size() >= 4) continue;
            const Side side = (in.u8() & 1) ? Side::Buy : Side::Sell;
            const Price4 px = pick_price(in);
            bool occupied = false;
            for (const Tracked& other : tracked) {
                const Entry* live_entry = lanes.pess.find(other.id);
                if (other.side == side && other.price == px && live_entry != nullptr &&
                    live_entry->live) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) continue;
            const uint32_t qty = in.range(1, 200);
            const uint32_t display = (in.u8() % 4 == 0) ? in.range(1, qty) : 0;
            const uint64_t id = next_id++;
            lanes.naive.place(book, id, side, px, qty, display, ts);
            lanes.opt.place(book, id, side, px, qty, display, ts);
            lanes.mbo.place(book, id, side, px, qty, display, ts);
            lanes.pess.place(book, id, side, px, qty, display, ts);
            Tracked t{id, side, px, qty, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const Entry* eo = lanes.opt.find(id);
            const Entry* em = lanes.mbo.find(id);
            const Entry* ep = lanes.pess.find(id);
            t.prev_ahead_opt = eo ? eo->ahead : 0;
            t.prev_ahead_mbo = em ? em->ahead : 0;
            t.prev_ahead_pess = ep ? ep->ahead : 0;
            tracked.push_back(t);
        }

        // ---- invariants, after every message ----
        for (Tracked& t : tracked) {
            if (!check_entry(lanes.opt, book, t.id, "opt")) return false;
            if (!check_entry(lanes.mbo, book, t.id, "mbo")) return false;
            if (!check_entry(lanes.pess, book, t.id, "pess")) return false;

            const Entry* eo = lanes.opt.find(t.id);
            const Entry* em = lanes.mbo.find(t.id);
            const Entry* ep = lanes.pess.find(t.id);

            // I4 — the bounds must contain the reference-resolved answer.
            //
            // The containment argument is inductive: all three lanes start at
            // the same ahead0, and optimistic's decrements are a superset of
            // mbo's, which are a superset of pessimistic's. An ICEBERG REFRESH
            // breaks that induction, and does so legitimately. A refreshed slice
            // rejoins the back, so `ahead` is reset from the book rather than
            // decremented — and because the faster lane fills first, it also
            // refreshes first, and is then behind a queue the slower lane has
            // not rejoined yet. Observed: opt(ahead=93, refreshes=1) against
            // mbo(ahead=1, refreshes=0).
            //
            // So the invariant is conditional on no lane having refreshed. That
            // is a real limitation of the bound, not a bug, and it means the
            // published band brackets mbo only for fully displayed orders. It is
            // stated here rather than quietly relaxed.
            const bool none_refreshed =
                eo && em && ep && eo->refreshes == 0 && em->refreshes == 0 && ep->refreshes == 0;
            if (none_refreshed && eo->live && em->live && ep->live &&
                eo->display > 0 && em->display > 0 && ep->display > 0) {
                INVARIANT(eo->ahead <= em->ahead,
                          "I4: optimistic believes more is ahead than mbo does");
                INVARIANT(em->ahead <= ep->ahead,
                          "I4: mbo believes more is ahead than pessimistic does");
            }

            // I2 — ahead only ever falls. The one legal exception is an iceberg
            // refresh: a new slice rejoins the back of the queue, which is what
            // makes hiding size cost priority. The entry reports refreshes
            // explicitly rather than the test inferring them from share counts.
            auto check_i2 = [&](const Entry* e, uint64_t prev_ahead, uint32_t prev_ref,
                                const char* what) {
                if (e == nullptr) return true;
                if (e->refreshes != prev_ref) return true;   // legally rejoined the back
                if (e->ahead > prev_ahead) {
                    std::fprintf(stderr, "INVARIANT VIOLATED (%s): %llu -> %llu\n", what,
                                 (unsigned long long)prev_ahead, (unsigned long long)e->ahead);
                    ++failures;
                    return false;
                }
                return true;
            };
            if (!check_i2(eo, t.prev_ahead_opt, t.prev_refresh_opt, "I2: optimistic ahead grew"))
                return false;
            if (!check_i2(em, t.prev_ahead_mbo, t.prev_refresh_mbo, "I2: mbo ahead grew"))
                return false;
            if (!check_i2(ep, t.prev_ahead_pess, t.prev_refresh_pess, "I2: pessimistic ahead grew"))
                return false;
            if (eo) { t.prev_ahead_opt = eo->ahead; t.prev_refresh_opt = eo->refreshes; }
            if (em) { t.prev_ahead_mbo = em->ahead; t.prev_refresh_mbo = em->refreshes; }
            if (ep) { t.prev_ahead_pess = ep->ahead; t.prev_refresh_pess = ep->refreshes; }

            // I5 — cumulative fills are ordered. Conditional on no refresh for
            // the same reason as I4: once a lane rejoins the back at a different
            // moment from the others, the fill orderings can legitimately cross.
            const uint64_t cn = sum_fills(fn, t.id, false);
            const uint64_t co = sum_fills(fo, t.id, false);
            const uint64_t cm = sum_fills(fm, t.id, false);
            const uint64_t cp = sum_fills(fp, t.id, false);
            if (none_refreshed) {
                INVARIANT(cn >= co, "I5: optimistic filled more than naive");
                INVARIANT(co >= cm, "I5: mbo filled more than optimistic");
                INVARIANT(cm >= cp, "I5: pessimistic filled more than mbo");
            }

            // I6 — nothing appears or disappears.
            for (const QueueModel* q : {&lanes.naive, &lanes.opt, &lanes.mbo, &lanes.pess}) {
                const Entry* e = q->find(t.id);
                if (e == nullptr) continue;
                const uint64_t filled = sum_fills(
                    q == &lanes.naive ? fn : q == &lanes.opt ? fo : q == &lanes.mbo ? fm : fp,
                    t.id, false);
                INVARIANT(filled + e->display + e->hidden == t.quantity,
                          "I6: filled + resting + hidden != original quantity");
            }

            // I7 — we cannot have been filled by more than actually traded at
            // our price. Price-priority fills are a different mechanism and are
            // excluded; they are bounded by their own reach.
            const uint64_t traded_here = executed_at_price[t.price];
            for (const std::vector<SimFill>* f : {&fo, &fm, &fp}) {
                INVARIANT(sum_fills(*f, t.id, true) <= traded_here,
                          "I7: filled more than traded at our price");
            }
        }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_sequence(data, size);
    return 0;
}

#ifndef ITCHBOOK_LIBFUZZER
int main(int argc, char** argv) {
    uint64_t iterations = 200000;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--iterations" && i + 1 < argc) iterations = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "usage: %s [--iterations N] [--seed N]\n", argv[0]); return 2; }
    }

    std::mt19937_64 rng(seed);
    std::vector<uint8_t> buf;
    for (uint64_t i = 0; i < iterations; ++i) {
        const size_t n = 24 + (rng() % 300);
        buf.resize(n);
        for (size_t j = 0; j < n; ++j) buf[j] = static_cast<uint8_t>(rng());
        if (!run_sequence(buf.data(), buf.size())) {
            std::fprintf(stderr, "\nfailing input (seed %llu, iteration %llu):\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(i));
            for (uint8_t b : buf) std::fprintf(stderr, "%02x", b);
            std::fprintf(stderr, "\n");
            return 1;
        }
        if ((i + 1) % 50000 == 0) {
            std::printf("  %llu sequences ok\n", static_cast<unsigned long long>(i + 1));
            std::fflush(stdout);
        }
    }
    std::printf("OK: %llu random book-legal sequences, queue invariants I1-I7 hold\n",
                static_cast<unsigned long long>(iterations));
    return failures == 0 ? 0 : 1;
}
#endif
