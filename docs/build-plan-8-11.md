# itchbook — Build Plan, Phases 8–11

**Written:** 21 August 2026
**Supersedes:** the "Phase 8 — Write-up" stub in `build-plan.md` (that write-up
shipped) and the first draft of this document. Writing is now continuous; the
numbered phases below are engineering.

**State at time of writing.** Phases 1–7 complete and verified: 13/13 tests pass
under ASan/UBSan on a fresh clone, the C++/Python differential is bit-identical
over a full generated day, the full-day differential runs on a real MSFT slice,
and the measurement harness does what the README claims. Known limits:
single-symbol tools, zero threads and zero atomics anywhere in the codebase, one
validated symbol-day (MSFT 2019-12-30).

**Calendar.** The March 2027 soft deadline is obsolete. Quant campus recruiting
for Summer 2027 runs Sept–Dec 2026 and DRW Montréal's posting is live now. The
order below is chosen so the CV strengthens at each milestone.

| Milestone | Target | Why the date |
|---|---|---|
| Phase 8 done | late Sept 2026 | biggest CV delta per week; on the CV before applications go out |
| Phase 9 v1 (ring + a wire-to-book number) | mid Oct 2026 | "multi-threaded, real-time" must be true-on-repo before first interviews |
| Phase 9 complete | Nov 2026 | the rate–latency curve is deep-dive material during interview season |
| Phase 10 paper | Dec 2026 – Feb 2027 | talkable in later rounds; no external deadline |
| Phase 11 closed loop | Feb – May 2027 | the terminal form; new-grad cycle Sept 2027 |

The done-condition discipline is unchanged: **every phase has a done-condition
you can fail, graded by something that isn't you.**

---

## 0. What the first draft of this document got wrong

This section exists because the plan is held to the same standard as the code.
Six claims in the draft were checked against the source and did not survive. Each
correction changes work items below, so they are recorded rather than quietly
fixed.

1. **`BookSet` cannot be `std::vector<Book>` as sketched.** `Book` owns its own
   `RefMap` and its own `Pool` (`book.hpp:707-708`), and `refs_capacity`
   defaults to `1<<20` slots (`book.hpp:396`) — 16 MB per book. Nine thousand
   books is ~144 GB of ref map before a single order exists. The shared ref map
   and shared pool are therefore **mandatory structural work**, not an
   optimisation to be measured later.

2. **The "shared vs per-book RefMap" experiment as stated is not runnable.**
   Per-book at 9,000 symbols only exists if each map is shrunk to a few hundred
   slots, which measures a different thing. Replaced in 8.9 by two experiments
   that can actually be run and can actually fail.

3. **`Order::locate` is not needed for routing.** Every ITCH message carries the
   stock locate in its common header (`messages.hpp:83-86`) — including `E`, `C`,
   `X`, `D` and `U`. The draft's justification ("a delete arriving with only a
   reference must recover which book") is wrong. Keep the field anyway: it costs
   nothing, it fits in the existing padding, and it turns into a **cross-check**
   that catches the bug class that would otherwise be invisible.

4. **`Side` does not support re-centring.** `set_band` returns early when the
   band already exists (`book.hpp:168`), so re-centring is new code, not policy
   on top of an existing API. Worse, the band is centred on the **first order's
   price**, not the first quote, and the first order of an ITCH day arrives
   around 04:00 pre-open. At today's `band_pct = 20` that is harmless; at the
   width a memory budget forces it is not.

5. **`sizeof(Level)` is 32, not 40** (`level.hpp:19`: 8+8+8+4+pad). The band
   arithmetic in the draft was 25% high. Numbers come from the source, the same
   way they come from `census` and not from a website.

6. **The throughput arithmetic contradicted its own premise.** The phase opens
   by saying the working set stops fitting in cache, then predicts full-day book
   time from the cache-hot single-symbol 43.9M msg/s. That number is now stated
   as an upper bound with a written prediction attached, and the run grades it.

One draft claim was checked and **confirmed**: a `uint16_t locate` fits inside
`Order`'s existing `uint8_t _pad[3]` and `sizeof(Order) == 40` survives.

---

## Phase 8 — Full day, all symbols (~4 weeks)

### Why this phase exists

1.22M messages is one symbol's slice. The feed is a few hundred million messages
across ~8–9k securities — run `itch_census` on the full file for the exact
numbers; every claim in this phase gets its number from `census`, not from this
document. Multi-symbol is not "the same thing but bigger": the working set stops
fitting in cache, memory becomes a budgeted resource instead of a rounding error,
and throughput becomes bound by something other than the book. Each of those is
the content.

### The work, in commit order

Each numbered item is one commit with its own check. They are ordered so the
tree builds and the tests pass at every step.

#### 8.0 — Census before code

Download `12302019.NASDAQ_ITCH50.gz` (and `01302019.NASDAQ_ITCH50.gz` while you
are there — phase 8 needs a second day and the download is the slow part). Budget
~4 GB compressed each and confirm free disk before starting.

Extend `itch_census` with the two numbers the rest of the phase is sized from:

- `--peak-orders`: high-water mark of live orders across the whole feed
  (adds + replaces, minus deletes, minus full executions). This sizes the shared
  ref map and it is the only honest way to get it.
- `--per-symbol`: message count, first/last quoted price and price range per
  locate, written as JSON. This sizes the bands and picks the sampled symbols.

Commit the JSON to `validation/census-2019-12-30.json`. Every memory number in
this phase's README section cites it.

**Check:** total message count from `--per-symbol` equals the type-total count
from the existing census path. Two ways of counting the same file must agree.

#### 8.1 — `Book` stops owning its storage

The structural commit, and the one everything else waits on. `Book` takes
`RefMap&` and `Pool&` rather than holding them:

```cpp
class Book {
public:
    Book(RefMap& refs, Pool& pool, uint16_t locate, int32_t tick, int32_t band_pct);
    // ...
private:
    RefMap& refs_;
    Pool&   pool_;
    uint16_t locate_;
```

Single-symbol callers construct one `RefMap` and one `Pool` alongside one `Book`,
so `book_replay --symbol` behaves exactly as before. Nothing else changes in this
commit — no locate field, no `BookSet`, no dispatch change.

**Check:** the whole existing suite, plus `full-day-differential.sh` on the MSFT
slice, still bit-identical. This commit must be provably invisible.

#### 8.2 — Per-symbol recovery on shared storage

`clear_orders()` currently does `pool_ = Pool(); refs_ = RefMap(...)`
(`book.hpp:412`). On shared storage that would wipe all 9,000 books and leak
every other symbol's nodes — it would silently destroy the phase-7 guarantee
rather than break a test. Replace it with a walk:

```cpp
void clear_orders() {              // erase only THIS book's orders
    bids_.for_each_order([&](Order* o){ refs_.erase(o->ref); pool_.deallocate(o); });
    asks_.for_each_order(/* same */);
    bids_ = Side('B', tick_);      // bands rebuilt lazily, as at session start
    asks_ = Side('S', tick_);
    resting_shares_ = 0;
}
```

**Check:** a new test — two books on one `RefMap`/`Pool`, fill both, clear one,
assert the other's every ref still resolves and its shares are unchanged; assert
`pool.live()` fell by exactly the cleared book's order count. Then re-run the
phase-7 adversarial scenarios: 0 WRONG, unchanged.

#### 8.3 — `Order` gains a locate, used as a cross-check

```cpp
uint8_t  side;       // 1
uint16_t locate;     // 2   <- was padding
uint8_t  _pad;       // 1
```

`static_assert(sizeof(Order) == 40)` holds; hot-path field placement is
untouched. Routing uses the **message header's** locate (`itch::stock_locate`),
not this field. This field exists so that on every `E`/`C`/`X`/`D`/`U` the book
can assert that the order the reference resolved to belongs to the symbol the
message claims. Count mismatches into a `locate_mismatch_` statistic and report
it in the summary.

**Why it earns its byte:** a reference collision, a hash bug, or a
last-writer-wins duplicate would otherwise mutate the wrong symbol's book and
produce a result that is individually plausible everywhere and wrong in
aggregate. This is the multi-symbol version of "nothing tells you when you are
wrong".

**Check:** `locate_mismatch == 0` across the full day. If it is not zero, that is
the phase's first real bug and it goes in the README.

#### 8.4 — The directory and the session

Parse `R` fully into `SymbolInfo` (symbol, market category, financial status,
round lot, ETP flag) — you currently skip most of `R`'s fields; stop skipping.

```cpp
// include/itchbook/book/book_set.hpp
class BookSet {
    RefMap                   refs_;    // one, shared, pre-sized from census
    Pool                     pool_;    // one, shared
    std::vector<Book>        books_;   // indexed by locate; sized from the R pass
    std::vector<SymbolInfo>  dir_;     // symbol, round lot, tick regime, from R
    SessionState             session_; // 'S' lives here, not in a Book
};
```

`S` (system event) is session-global and carries locate 0. Routing it by locate
would put it in book 0 and leave 8,999 summaries without session state — which
is a silent corruption of 8,999 outputs, not a crash. It goes in `SessionState`
and every summary reads it from there.

Books are constructed lazily on a symbol's first message so that the ~1,000
locates that never trade cost a `SymbolInfo` and nothing else.

**Check:** `dir_` size equals the `R` count from census; every locate seen in a
message body has a directory entry (a message for an undirectoried locate is a
framing bug and must be counted, not ignored).

#### 8.5 — Dispatch routes on locate; halts go per-symbol

`dispatch.hpp` keeps its shape — it stays the only file that knows both sides,
and `apply()` must still mirror `python/reference/book.py` case for case. It
gains one line at the top that resolves `BookSet::at(itch::stock_locate(p))`,
and `S` diverts to the session.

`H` (stock trading action) is per-symbol: the `recover/halt.hpp` state machine
moves into `SymbolInfo` (or a parallel array indexed by locate). A halt on one
symbol must not gate trading on another — today it would.

**Check:** the oracle's `apply()` is updated in the same commit, and the sample
differential passes. If the two files stop being readable side by side, the
commit is wrong.

#### 8.6 — `book_replay --all-symbols`, and the regression gate

`--symbol X` still works and must produce **byte-identical output to the phase-7
repo**. Freeze the current MSFT snapshot CSV and summary JSON into
`validation/regression/` and make the comparison a CI step. Everything in this
phase is a refactor of the hot path; this gate is what says the refactor was
free.

`--all-symbols` writes one summary row per symbol and, optionally, snapshots for
a named subset (snapshotting 9,000 books at 1 Hz would produce more output than
input).

**Check:** the regression gate, green, in CI, on every push for the rest of the
project.

#### 8.7 — The band policy

The phase-3 story — "the levels near the touch stay in L1" — cannot survive 9,000
books unmodified, and *showing that you know that* is the point.

The arithmetic, which goes in the README with `sizeof(Level) == 32` cited: a
fixed 512-level band per side costs `9,000 × 2 × 512 × 32 ≈ 295 MB` of bands
alone, before orders, before the ref map. A ±2% band on a $100 stock at penny
ticks is 200 levels per side; the same percentage on a $1,000 stock is 2,000.
Percentage-width bands mean memory you cannot state in advance, so:

1. **Width is a tick count, not a percentage**, chosen from a stated budget.
   `--band-levels N` (default to be set by the sweep, start at 512). Memory
   becomes `active_symbols × 2 × N × 32` and is knowable before the run.
2. **Lazy init stays**, but centres on the first **two-sided quote**, not the
   first order. The first order of the day is a 04:00 pre-open order and is
   routinely far off-market. Until a band exists everything falls through to the
   overflow `std::map`, which is correct, just slow — and now measured.
3. **One re-centre allowed per symbol per session**, triggered when off-band adds
   exceed 10% of that symbol's adds over its first 1,000 adds. Re-centring
   rebuilds the dense array and re-indexes every resting order, so it is not
   free and it is not silent: count re-centres and report how many symbols
   needed one.
4. **Count overflow hits per symbol and report the distribution.** If a symbol
   lives in its overflow map, the band policy failed for that symbol and the
   README says which symbols and why. Prediction to write down first: the
   failures are the high-priced names (a $1,800 stock at penny ticks cannot be
   covered by any band you can afford), and the honest conclusion is that a
   production system uses a per-symbol tick regime rather than one global grid.

#### 8.8 — Sizing the shared ref map and pool

1. **Pre-size the ref map** to the next power of two above `2 × peak_orders`
   from 8.0. State plainly in the README that a replay knows the future and a
   live system does not; the alternative — grow online, and let the tail
   histogram contain multi-hundred-millisecond rehash spikes — is named, and the
   spikes are what pre-sizing buys you out of. Do the first, note the second.
2. **Re-run the phase-4 chunk sweep at full-day scale.** The 4,096 → 262,144
   doubling policy was tuned on one symbol's order flow; the cap probably moves.
   Phase-4 methodology applies verbatim: pin, interleave, ≥5% or it is not a
   result.

#### 8.9 — The two experiments that replace the draft's unrunnable one

Predictions written down before either is run:

- **Shared pool vs per-book pools.** Prediction: the shared pool *wins*, because
  allocation order tracks arrival order, so orders that arrive together sit
  together and a cache line pulled in for one message is warm for the next. The
  per-book variant is only runnable with a tiny first chunk (4,096 orders ×
  9,000 books is 1.4 GB of mostly-empty slab), and that shrinkage is part of what
  is being measured. Report it that way.
- **Ref map load factor.** Prediction: flat between 25% and 50% load, because the
  probe is one cache line either way and the table is far past L2 in both cases.
  A flat result here is a result and gets reported, not deleted.

#### 8.10 — The framing path

Before blaming zlib for anything, measure the framing. `Reader::next()`
(`reader.hpp:46-62`) makes **two `gzread` calls and a `vector::resize` per
message** — at full-day scale that is hundreds of millions of zlib entry points.
Batch the reads into a buffer and re-measure. This is inside the no-third-party
rule and may be the largest single win in the phase; it is certainly the
cheapest.

**Check:** decompress-only throughput (MB/s) reported separately from
parse-only and book-only, so the three are attributable.

#### 8.11 — Throughput, honestly

Do this arithmetic in the README **before** showing any full-day number, and
attach the prediction:

The book does 43.9M msg/s on a cache-hot single-symbol feed, so N messages is an
**upper bound** of N/43.9M seconds of book work. Written prediction: multi-symbol
throughput will be materially below that — random access across thousands of
bands and a ref map far past L2 — and the honest guess is a 2–3× degradation.
Single-threaded zlib inflates at a few hundred MB/s and the uncompressed day is
on the order of 10 GB, so decompression is tens of seconds. Whether the run is
decompression-bound, and by how much, is therefore a *measurement*, not a claim
this document is entitled to make.

Consequences:

1. Report two numbers, clearly labelled: **end-to-end from the .gz** and
   **book-only from pre-decompressed input**. Conflating them is the kind of lie
   this repo exists to not tell.
2. The no-third-party-deps rule stays (no libdeflate). The legitimate fix inside
   the rule is overlap: a reader thread decompressing ahead into buffers while
   the book consumes. **Do not build it here** — it is a ring buffer with two
   threads, which is phase 9. Ship the two honest numbers and one sentence saying
   which phase closes the gap. A plan that predicts its own next bottleneck is
   worth more than a fix.
3. Memory instrumentation: peak RSS from `/proc/self/status` `VmHWM` at exit,
   plus per-structure accounting (bands, pool, ref map, directory) so the total
   decomposes and the residual is visible.

#### 8.12 — Verification at scale

The oracle cannot chew a full day — its job description is "slow, obvious,
correct." The method changes shape; the standard does not.

1. **Sampled differential.** Seeded random selection of K symbols (K ≥ 8, always
   including MSFT and one ETF), `itch_slice` each, full oracle differential on
   each: bit-identical snapshots and summaries, same as today. The seed is
   printed so a run reproduces; CI pins one seed, local runs rotate.
2. **Global invariants**, in one full-day pass, against `itch_census` run
   independently: per-type messages consumed == census counts; Σ per-symbol
   volume == total executed volume; final live orders == adds − deletes − fully
   executed; `unknown_ref == 0` and `locate_mismatch == 0` across the entire
   feed. These catch the class of bug sampling misses — correct per symbol,
   wrong in aggregate.
3. **External oracle, plural.** Databento `XNAS.ITCH` daily bars, exact on all
   five fields, for ≥5 symbols spanning the liquidity spectrum: a mega-cap, a
   mid-cap, an ETF, something illiquid, and something that halted that day if
   the day has one. One symbol proves the parser; five prove the system.
   `validate.py`'s UTC-window rule applies to every one of them.
4. **A second day.** `01302019.NASDAQ_ITCH50.gz` is public. A different day is a
   different distribution — band policy, pool cap and halt handling all get
   exercised differently. Same invariants, same bars.
5. **CI, which has no market data.** Extend `make_queue_feed.py` with
   `--locates N` to emit a multi-symbol synthetic feed (interleaved locates,
   per-symbol halts, cross-symbol ref uniqueness, one deliberately high-priced
   symbol so the band policy is exercised), and run the multi-symbol differential
   on it every push. `fuzz_feed.py` already emits a second locate
   (`fuzz_feed.py:41`) — generalise that path rather than writing a new
   generator.

#### 8.13 — `docs/phase8-results.md`, and the README's top block

Written the same week the numbers land, not later.

### Done — Phase 8

- [ ] Full public day, all symbols, one process: end-to-end and book-only wall
      clock both reported, with the bottleneck attributed by measurement.
- [ ] The written throughput prediction kept or falsified, in print.
- [ ] ≥8 randomly sampled symbols bit-identical vs the oracle (seed printed);
      global invariants hold against an independent census; `unknown_ref == 0`
      and `locate_mismatch == 0`.
- [ ] ≥5 symbols exact vs Databento bars; repeated on a second trading day.
- [ ] Peak RSS reported and decomposed; band overflow-hit distribution and
      re-centre count reported; the band-budget paragraph written.
- [ ] `--symbol MSFT` byte-identical to the phase-7 repo (regression gate in CI).
- [ ] Shared-pool and load-factor experiments run with predictions written first.

**CV line unlocked:** "Replays a full NASDAQ trading day — N million messages,
~8,700 symbols — through one process in X s (Y s book-only), verified by sampled
differential against an independent oracle and exact against Databento daily bars
on five symbols across two days."

---

## Phase 9 — The pipeline: wire-to-book (~5 weeks)

### Why this phase exists

Every latency number in the repo today is *handler cost*: cycles between
`cycles_begin()` and `cycles_end()` around a function call. There is no thread
boundary, so there is no number resembling what a trading system means by
latency — time from bytes arriving to book updated. This phase creates the
boundary, measures across it honestly, and produces the artifact interviews are
actually about: the rate–latency curve and the story of where it cliffs. It also
removes phase 8's decompression gap as a side effect.

### 9.1 Topology

Three pinned actors, three processes — processes, not threads, for the replayer,
so its jitter never shares a scheduler decision with the system under test:

```
mold_replay_udp  --UDP-->  receiver thread --ring--> book thread
(paced sender)             (recvmmsg, tstamp)        (parse, apply, tstamp)
```

Needs four real cores. Confirm that before starting; if the dev machine has two,
the numbers are noise and the phase needs different hardware.

### The work, in commit order

#### 9.0 — `docs/phase9-methodology.md`, written first

Before any measurement code, in the spirit of "the measurement mistake that came
first":

1. **Coordinated omission.** If the sender paces off completions or stalls when
   the receiver slows, the distribution lies — slow periods generate fewer
   samples exactly when latency is worst. The sender therefore sends on a fixed
   schedule and records *intended* send time; lateness of the send itself is
   measured and reported separately. Name the concept; it is the canonical trap
   and naming it is half the credit.
2. **Clock domain — and the part the draft got wrong.** The plan of record was
   "arrival and book-done both come from the receiver's TSC, one clock, no sync
   problem." The TSC is **per-core**, and this is the first time the repo
   subtracts a stamp taken on one core from a stamp taken on another.
   `tsc_is_invariant()` (`rdtsc.hpp:64`) checks `constant_tsc`/`nonstop_tsc` —
   it says nothing about the offset between cores. So: measure the cross-core
   offset with a ping-pong test, report it, and if it is not negligible against
   the latencies being measured, use `CLOCK_MONOTONIC_RAW` for the cross-thread
   sample and keep the TSC for intra-thread work. State which one each table
   used.
3. **Loopback is not a network.** No NIC, no interrupt coalescing, no kernel
   bypass. The number is "wire"-to-book on loopback; the doc says what would
   change on real hardware, and that hardware timestamping and DPDK are out of
   scope by design.
4. **Pinning.** Three actors, three cores, `taskset` documented; interleaved A/B
   runs for any before/after claim, exactly as `bench/compare.py` already does.

#### 9.1 — `include/itchbook/pipe/spsc_ring.hpp`

No third-party lock-free library — the entire point is that you can defend every
line.

```cpp
template <typename Slot, size_t CapacityPow2>
class SpscRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0);
    alignas(64) std::atomic<uint64_t> head_{0};   // producer writes
    alignas(64) std::atomic<uint64_t> tail_{0};   // consumer writes
    alignas(64) uint64_t cached_tail_{0};         // producer's stale copy
    alignas(64) uint64_t cached_head_{0};         // consumer's stale copy
    std::array<Slot, CapacityPow2> slots_;
};
```

Each decision is a comment block in the header, in the house style:

1. **Indices are monotonically increasing u64**, masked on access. Wraparound of
   the index itself is a centuries-scale event at feed rates; say so and move on.
   Empty is `head == tail`, full is `head − tail == Capacity` — no wasted slot,
   no separate count.
2. **Acquire/release and nothing stronger.** Producer: `head_.store(h+1, release)`
   *after* writing the slot; consumer: `head_.load(acquire)` before reading it.
   Write down why relaxed on the index publish would be wrong (the slot store
   must happen-before the publish, and only release/acquire gives you that edge)
   and why `seq_cst` buys nothing here. That paragraph is a guaranteed interview
   question.
3. **`alignas(64)` on each index** so producer and consumer never bounce a shared
   line. Predicted-flat candidate to actually test: padding to 128 B for
   adjacent-line prefetchers.
4. **Cached counterpart indices.** The producer re-reads `tail_` only when its
   cached copy says full; ditto the consumer. Most pushes and pops become zero
   cross-core traffic. Measure on and off — this is the ring's phase-4 story.
5. **Batched publication.** The producer writes a whole `recvmmsg` batch of slots
   then publishes `head_` once: one release store per 32 messages.
6. **Slot layout: fixed 64 bytes** — arrival timestamp (8) + raw ITCH bytes (≤50
   for every type handled) + length (2). The producer does framing only; **the
   consumer parses**. Rationale to write down: it keeps the producer's
   per-message work near `memcpy` so the receiver can absorb bursts, and keeps
   parse cost inside the measured region where it belongs.

**Check (this commit):** unit tests for empty/full boundaries, wrap across the
u64 mask, and FIFO order preserved across 2^32 pushes — the masked-index bug
hunt.

#### 9.2 — The TSan job

TSan and ASan cannot share a build, so this is a third CI job:
`-fsanitize=thread`, plus a stress run with randomised producer/consumer stalls,
100M ops, sanitizer-clean. Land it with the ring, not after it — a lock-free
structure that has never been under TSan is an assertion, not a result.

#### 9.3 — `tools/mold_replay_udp.cpp`

You have `mold_wrap` (ITCH → MoldUDP64) and `mold_replay`; this adds a socket and
a pacer. Pacing is a token bucket over **messages**, not packets, with `--rate`
in msg/s and `--multiplier` over the feed's own timestamps. The sender records
its intended schedule and reports send lateness separately (9.0, item 1).

#### 9.4 — The receiver thread, and the drops that are not yours

`recvmmsg` with a batch of 32, one `cycles_begin()` per batch (amortised;
per-packet `SO_TIMESTAMPNS` kernel stamps are the stretch goal, and the README
says which one a given table used). `SO_RCVBUF` raised, and the achieved value
verified with `getsockopt` — Linux silently caps it.

**The part the draft missed:** on loopback the kernel's socket buffer overflows
before your ring ever fills. If you only count ring-full events, the 9.5 story
("backpressure degrades to graded feed gaps") has a hole you did not look in —
the losses are real, they are upstream, and they are invisible. So read
`/proc/net/udp` drop counts and the `recvmmsg` receive-error counters, and report
**kernel drops and ring drops separately** in every table. A system that knows it
is drowning has to know where the water came in.

#### 9.5 — The book thread, and backpressure as a gap

Pop, parse, apply to the phase-8 `BookSet`, `cycles_end()` after apply. The sample
is `(apply_done − arrival)`, into one preallocated `Histogram` per message type
plus one overall. All phase-4 rules apply inside the loop: no allocation, no I/O,
no printing.

When the ring is full the receiver does not block and does not spin unboundedly:
it drops the **packet** and counts it. A dropped MoldUDP64 packet is precisely a
sequence gap, which is precisely what `recover/gap_policy.hpp` and the phase-7
grader exist for.

1. Ring-full → packet drop → sequencer sees the gap → existing policy runs
   (recovering / halted verdicts).
2. New adversarial scenario in `adversarial.py`: `consumer-slow`, with the book
   thread artificially throttled. Graded on the existing CORRECT/SAFE/WRONG
   scale; **0 WRONG** is the bar, same as every other scenario.
3. The kill switch gets a new input: sustained ring occupancy above a threshold.
4. **One caveat to fix or state:** the sequencer's reorder buffer is a
   `std::map` (`sequencer.hpp`), so it allocates on the producer path exactly
   when the system is stressed — a violation of the phase-4 loop rules at the
   worst possible moment. Either give it preallocated storage or write the
   exception down in the methodology doc. Do not let it pass unnoticed.

#### 9.6 — The determinism gate

At rates below the knee with drops == 0, the resulting book must be
byte-identical to synchronous `book_replay` on the same input. The pipeline may
reorder *time*; it may never reorder *effect*. This is a CI gate.

Plus a torture test: producer pinned faster than consumer for sustained periods;
assert every message is either applied exactly once or accounted as a graded
drop. Shares conserved across the boundary.

#### 9.7 — The headline artifact: the rate–latency curve

Sweep `--rate` from real-time — compute the real number from census timestamps,
do not quote a figure from this document — up through 10×, 100×, to the cliff.
Plot p50/p99/p99.9 wire-to-book against offered rate with the existing
`svgchart.py`. Two required annotations: the knee where queueing delay appears,
and the **max sustainable rate** (highest rate with zero ring-full drops *and*
zero kernel drops over a full day). The second number is the CV number.

#### 9.8 — Close phase 8's gap

The reader path becomes a thread decompressing ahead into buffers while the book
consumes — the same ring, a different slot type. Update phase 8's end-to-end
full-day number in the README and say which phase moved it.

#### 9.9 — `docs/phase9-results.md`

### Done — Phase 9

- [ ] Wire-to-book p50/p99/p99.9 at 1× real-time and at max sustainable rate,
      from a TSan-clean, pinned, methodology-documented run.
- [ ] Cross-core TSC offset measured and reported, or `CLOCK_MONOTONIC_RAW` used
      and said so.
- [ ] Rate–latency curve with knee and cliff annotated; max sustainable rate in
      msg/s; kernel drops and ring drops reported separately at every rate.
- [ ] Ring-full events land as phase-7 gaps; `consumer-slow` graded, 0 WRONG.
- [ ] Below-knee pipeline output byte-identical to synchronous replay (CI gate).
- [ ] Cached-index and batched-publish measured with predictions written first;
      one predicted-flat candidate tested and reported.
- [ ] Phase 8's decompression gap closed and the full-day number updated.

**CV line unlocked:** "Lock-free SPSC pipeline (hand-written ring,
acquire/release, cache-line-isolated indices): wire-to-book p50 X ns / p99.9 Y
ns, sustains Z M msg/s on loopback, with backpressure degrading to graded feed
gaps rather than silent loss."

---

## Phase 10 — The market-making paper (~8 weeks, interleaves with interviews)

### Why this phase exists

Phase 6 measured how fills lie. This phase uses that machinery to evaluate a real
strategy from the literature — Avellaneda–Stoikov (2008) — with the honesty the
rest of the repo is built on. The deliverable is an 8–12 page paper in `docs/`,
not a P&L screenshot. The likely conclusion is that the strategy loses money on a
public feed; a paper that shows *why*, mechanically, is worth more than a fake
win, and `what-synthetic-data-hides.md` proves you already know that.

**Note the cost before starting.** Every phase up to here has a grader that is not
you: Databento, the Python oracle, ASan, TSan, the adversarial scale. Phase 10
has none. That is a real step down in the property this repo sells, and the
mitigations — pre-registered predictions, an outside reader, a limitations
section that is longer than the results section — are what stand in for an
oracle. Treat them as done-conditions, not garnish.

### 10.0 The feedback wall comes down, deliberately

`strategy.hpp` structurally denies strategies their fills and position, so that
one intent stream feeds all four fill models and the P&L difference is
attributable to the model alone. **A-S cannot live behind that wall** — its
reservation price is a function of inventory `q`. So the wall comes down for this
phase, and the paper documents the trade:

1. New interface alongside the old: `InventoryStrategy` receives a `FillEvent`
   stream and its running position. The old `Strategy` interface and every
   phase-6 result remain untouched.
2. The four-model band changes meaning: it becomes **four closed-loop runs**, one
   per fill model, each internally consistent — the strategy in the pessimistic
   lane *sees* pessimistic fills and quotes accordingly. The band is a band over
   worlds, not over gradings of one world. That paragraph, written correctly in
   print, is the differentiator.
3. Risk limits stay in the harness (position caps clip intents), but the strategy
   now sees position, so the phase-6 rationale comment in `strategy.hpp` gets a
   sequel pointing here.

### 10.1 The strategy

`include/itchbook/sim/as_maker.hpp`, finite-horizon Avellaneda–Stoikov:

- Reservation price: `r(s,q,t) = s − q·γ·σ²·(T−t)`
- Total spread: `δᵃ + δᵇ = γ·σ²·(T−t) + (2/γ)·ln(1 + γ/k)`, split symmetrically
  around `r`
- Quotes clipped to the tick grid and to a max distance from mid; re-quote on a
  threshold move of `r` or on a fill, not on every tick — count re-quotes, they
  are a cost.

`σ` estimated online (rolling realised variance of mid, window a parameter). `T`
is session end. `γ` is swept, not chosen. Worth one section if time allows: the
Guéant–Lehalle–Fernandez-Tapia inventory-bounded variant, which fixes A-S's
end-of-horizon pathology — flag the pathology either way.

### 10.2 Calibrating λ(δ) from your own fills — the centrepiece

A-S needs `λ(δ) = A·e^(−k·δ)`. Everyone else assumes `A` and `k`; you can
*measure* them, because the MBO-resolved model knows actual fills and actual
queue-position-aware exposure.

1. Instrument the backtester to log, per resting order, `(depth δ from mid at
   each moment, exposure time at that depth, filled-or-not)`. Bucket by δ in
   ticks.
2. `λ̂(δ) = fills(δ) / exposure-time(δ)`. Fit log-linear; report `A`, `k`, `R²`
   and the residual plot. If the exponential form fits badly at the touch — it
   will, because queue position dominates at δ=0 — **say so**: that misfit is a
   known limitation of A-S and now you have the figure that shows it.
3. Calibrate on 2019-12-30, freeze, test elsewhere. Calibration and evaluation
   never share a day.
4. **The subtlety to state explicitly:** λ̂ is measured *through* a queue model,
   so `A` and `k` are conditional on that model. Calibrating under the
   MBO-resolved lane and then running four lanes means three lanes use parameters
   fitted in a different world. Either re-calibrate per lane (defensible, more
   work, and the band then means something cleaner) or calibrate once and state
   the conditioning in the methodology. Decide in writing; do not leave it
   implicit.

### 10.3 Experimental design

1. **Symbols:** ≥3 across the liquidity spectrum (MSFT plus a mid-cap plus an
   ETF). Per-symbol results reported separately; no aggregate-only tables.
2. **Days:** calibrate on one, test on ≥2 others (the second public sample day
   plus Databento credits for one more if the free files run out).
3. **Baseline:** the phase-6 symmetric touch-maker under the same closed-loop
   protocol. The headline question is honest and small: *does inventory-aware
   quoting lose less than naive symmetric quoting, and through which mechanism* —
   fewer toxic fills (markout improvement) or smaller inventory excursions
   (variance reduction)?
4. **Every headline number is a band** across the four models; γ-sweep as a
   figure, not cherry-picked; latency sensitivity via the existing
   `latency_sweep`. Prediction to write down before running: A-S re-quotes often,
   so it degrades *faster* with latency than the baseline.
5. Fees on (real NASDAQ schedule, already implemented), markouts at 100 ms / 1 s
   / 10 s, inventory path plotted, max drawdown reported.
6. **Statistical honesty:** with a handful of symbol-days there is no
   significance to claim. Frame it as a mechanism study with a day-level
   sensitivity table, and say in Limitations that N is small. Overclaiming here
   would undo the repo's whole brand.

### 10.4 The paper

`docs/paper/as-on-itch.md` → PDF. Abstract / Data & venue / Fill models
(condensed from phase 6) / The feedback-wall change and what the band now means /
Strategy & calibration / Results / Latency sensitivity / Limitations (public
displayed feed only — no hidden liquidity, single venue, small N, loopback
latencies) / Conclusion. Every figure regenerated by one script from committed
JSON; CI runs the script.

### Done — Phase 10

- [ ] λ(δ) calibrated from own MBO fills with fit quality and the touch-misfit
      figure shown; the conditioning decision from 10.2.4 written down.
- [ ] A-S vs baseline: ≥3 symbols × ≥3 days (calibration day excluded) × 4
      closed-loop fill models; all headline numbers are bands.
- [ ] The band-over-worlds methodology paragraph written and reviewed by someone
      who did not write it.
- [ ] Latency-degradation prediction written before the sweep, kept or falsified
      in print.
- [ ] Paper PDF builds from committed sources; one script regenerates every
      figure; CI runs it.

**CV line unlocked:** "Calibrated and evaluated an Avellaneda–Stoikov market
maker on real NASDAQ order flow with fill-model uncertainty bands — fill
intensity measured from queue-position-resolved fills rather than assumed."

---

## Phase 11 — OUCH order entry and the closed loop (~10 weeks)

### Why this phase exists

Today the matcher is a library called by a backtest. Real order flow enters an
exchange through a session protocol, and the exchange *publishes* what it did.
Implementing NASDAQ's inbound protocol (OUCH) and making the matcher emit ITCH
closes the loop: your strategy speaks OUCH to your exchange, your exchange
publishes ITCH, your own phase-9 pipeline consumes it, the strategy reacts. Every
hop is code you wrote; every hop is timestamped; the end-to-end number is
**tick-to-trade**.

### 11.0 The design decision that has to be made first

The draft's topology — "replay the recorded day's orders into the matcher as
exchange-side flow, so the strategy trades inside a real day" — does not work,
and the reason is not the one the draft names.

Feeding a historical day's ITCH `A` messages into your matcher **does not
reproduce that day even with zero strategy orders**. Historical adds were
non-marketable against the *real* book at the instant they were entered. Entered
against *your* book — which has already diverged, because ITCH's executions are
facts you would be discarding rather than crossing events you can replay —
orders cross that never crossed. The emitted stream then diverges from the real
day for reasons that have nothing to do with your strategy, and 11.4's
"backtester modelling error" table gets dominated by that artifact.

**The topology that works is hybrid:**

- Historical flow is replayed as **book state**, through the phase-8 path you
  already trust. Historical orders are never matched; they are applied.
- **Only your own orders** go through the gateway and the matcher, and they match
  against that state.
- The matcher emits ITCH for **its own** mutations, which merges into the
  published stream your strategy consumes.

You lose the sentence "real flow trades against my venue." You keep an experiment
whose disagreements mean something. Make this choice explicitly in
`docs/phase11-design.md` before writing the gateway, because it determines what
the gateway is for.

### 11.1 Protocol scope

`include/itchbook/ouch/` — pick one OUCH version (4.2 is simpler; state the
choice), implement the core subset, and list deviations from the spec in the
header the way `messages.hpp` does for ITCH:

- Inbound: Enter Order `O`, Replace Order `U`, Cancel Order `X`.
- Outbound: System Event `S`, Accepted `A`, Replaced `U`, Canceled `C`,
  Executed `E`, Rejected `J`.
- Fixed-width, alpha-padded, big-endian where the spec says so — same
  field-offset table style as the ITCH side, same trap-hunting mindset. The
  token/ref distinction is this protocol's version of the locate trap.

**Transport:** minimal SoupBinTCP underneath — login request/accept/reject,
sequenced data packets, client and server heartbeats, end of session. It is a
small protocol (2-byte length + 1-byte type framing) and implementing it upgrades
the claim from "parses OUCH structs" to "implements the session layer real firms
log into." Heartbeat timeout wires into the kill switch: a dead session flattens.

### 11.2 The gateway and the emitting matcher

1. `engine/gateway.hpp`: accepts a SoupBin session, validates OUCH messages
   (unknown token, bad price, wrong state → `J` with reason codes), assigns
   exchange-side order references, maintains the token↔ref map, and applies the
   risk layer **before** the matcher — `risk/kill_switch.hpp` in-line: price
   collar against the current book, max order rate, max position. A tripped
   switch rejects new flow and cancels resting orders; that behaviour is a test,
   not a hope.
2. **The matcher emits ITCH.** Every mutation produces the corresponding message
   — `A`/`F` on accept, `E` on execution, `X` on partial cancel, `D` on delete,
   `U` on replace, `P` for non-displayed executions if hidden slices trade —
   sequenced, timestamped, wrapped in MoldUDP64 by the existing `mold` layer,
   published on UDP. Your exchange now produces the wire format it consumes,
   which is the sentence that makes the project title honest.
3. Determinism: fixed seed + fixed inbound script ⇒ byte-identical ITCH output.
   Every test below leans on this property.

### 11.3 The loop and the number

Strategy process (phase-9 receiver + ring + book on the ITCH side, SoupBin client
on the OUCH side, `InventoryStrategy` from phase 10 in the middle) ⇄ exchange
process (gateway + matcher + ITCH publisher), with the historical-state replayer
as the third process per 11.0.

Timestamps at every hop, one histogram per hop, so the total decomposes:

- **t₀** market-data packet arrival at the strategy socket → **t₁** book updated
  (phase 9's number)
- **t₁ → t₂** strategy decision (intent computed)
- **t₂ → t₃** OUCH Enter written to socket — **t₀→t₃ is tick-to-trade**, the
  headline
- **t₃ → t₄** gateway accept; **t₄ → t₅** match + ITCH publish; **t₅ → t₆** own
  fill observed back at the strategy — the full round trip.

Report the decomposition as a stacked bar at p50 and p99.9. Any hop you cannot
explain, you have not finished.

### 11.4 The validation experiment that makes it a system

**Replay-vs-live A/B.** Run the identical historical day and identical strategy
twice: (a) through the phase-6/10 backtester, (b) live through the loop, with the
latency model in (a) set to the *measured* hop latencies from (b). Diff the fills.
The differences are precisely the backtester's modelling error — queue
approximation, latency-model shape, tie-breaks — and now they are enumerable.
`docs/phase11-results.md` is built around that table.

Caveats stated up front, both of them: your orders consume liquidity the
historical participants never saw (no market impact model in either lane), and
under the 11.0 hybrid the historical participants never react to you at all.
Perfect agreement is not expected; **explained** disagreement is the bar.

### 11.5 Testing

1. **Cross-protocol differential**, house style: every OUCH `E` has a matching
   ITCH `E`/`P`; token-level shares conserved across both streams; the book built
   by replaying the *emitted* ITCH equals the matcher's internal book — the
   emitted feed graded by your own phase-3 machinery, so the loop grades itself.
2. `tests/fuzz/fuzz_gateway.cpp`: random valid and invalid OUCH byte streams —
   malformed lengths, unknown tokens, replace-after-fill races, cancel-of-cancel.
   Invariants: no crash under ASan/UBSan, every inbound message answered exactly
   once, ITCH/OUCH consistency holds. CI runs 1M ops.
3. SoupBin session tests: heartbeat timeout → kill switch → flatten, proven by a
   test that counts the cancels.
4. Determinism gate in CI: fixed script ⇒ byte-identical emitted ITCH.

### Done — Phase 11

- [ ] Strategy trades against your own exchange over real sockets, inside a
      replayed historical day, under the 11.0 topology.
- [ ] Tick-to-trade p50/p99.9 reported and decomposed per hop; every hop
      explained.
- [ ] Cross-protocol differential and gateway fuzz (≥1M ops) clean in CI;
      deterministic emitted-ITCH gate green.
- [ ] Kill-switch flatten-on-trip and flatten-on-session-death proven by tests.
- [ ] Replay-vs-live A/B table published with every disagreement categorised.

**CV line unlocked:** "Protocol-complete exchange system: OUCH 4.2 over
SoupBinTCP into my own matching engine, which publishes ITCH 5.0 consumed by my
own feed handler — tick-to-trade measured end-to-end at p50 X µs, every hop
decomposed, backtester validated against the live loop."

---

## Standing rules (all phases)

1. **The drill.** Twice a week minimum through interview season: re-implement one
   core structure from a blank file, unassisted, timed — `RefMap` with
   backward-shift delete, a price `Level` with intrusive unlink, the SPSC ring
   once phase 9 exists. The repo is a promise about what you can do live; the
   drill keeps the promise current. Same for the stories: the pool-slab
   diagnosis, the 19.3% unpinned variance, the ring's memory-ordering argument —
   out loud, whiteboard, no notes. **This is the schedule. The phases fill the
   time around it.** Code that lands in a weekend and understanding that lands in
   a weekend are different things, and only one of them is examined in a room
   with no AI in it.
2. **Prediction before measurement.** Every optimisation gets its prediction
   written down first; flat results get reported, not deleted. "Two predicted
   wins that were not" has a sequel in every phase.
3. **Two honest numbers beat one flattering one.** Wherever a measurement has a
   caveat — decompression-bound, loopback, replay-only pre-sizing, kernel drops —
   the caveat sits in the same table as the number.
4. **Writing is continuous.** Each phase lands with its `docs/phaseN-results.md`
   before the next phase starts; the README's top block is updated the same day.
5. **CI is the referee.** Every done-condition that can run without licensed data
   runs on every push; the ones that cannot (full day, Databento) get a committed
   results JSON and a script that re-checks it against the repo's claims.
6. **Apply on milestones, not on completion.** September applications go out with
   phase 8 on the CV. The plan continuing is not a reason to wait.
