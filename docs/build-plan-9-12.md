# itchbook — Build Plan, Phases 9–12

**Version 2.0.** 21 August 2026.
**Supersedes** `build-plan-8-11.md` (v1.0, this file's previous name) and the
original draft before it. Writing is continuous; the numbered phases here are
engineering.

---

## 0. Why the numbers moved

v1.0 called these phases 8–11. The repository has since shipped its own phase 8 —
the write-up — and `README.md` now opens with **"Phases 1–8 complete."** Two
different phase 8s in one repository is exactly the kind of drift the last five
commits were spent removing from the docs, so the engineering phases renumber
once, here, and never again.

| v1.0 | v2.0 | What it is |
|---|---|---|
| Phase 8 | **Phase 9** | Full day, all symbols |
| Phase 9 | **Phase 10** | The pipeline: wire-to-book |
| Phase 10 | **Phase 11** | The market-making paper |
| Phase 11 | **Phase 12** | OUCH order entry and the closed loop |

---

## 1. State at time of writing — measured, not estimated

Phases 1–8 complete. Two CI jobs (clang+sanitizers, gcc), 11 unit tests plus two
property fuzzers, the phase-4 regression gate, the adversarial grader, and the
latency-histogram render path all run on every push.

The single biggest change since v1.0 is that **the full day has been run**:

| | |
|---|---:|
| messages in `12302019.NASDAQ_ITCH50.gz`, every symbol | **268,744,780** |
| uncompressed | **8.25 GB** |
| length mismatches | **0** |
| distinct message types present | 18 |
| modelled | 12 |
| framed and length-checked, not interpreted | 6 |
| messages not modelled | 4,248,527 — 1.58% |

Every arithmetic claim in this document now cites that census instead of
estimating. The v1.0 draft said "~370M messages" and "on the order of 12 GB";
both were wrong, and the file was sitting on disk the whole time. That is the
plan's own version of typing a number into Markdown instead of reading it off an
artifact, and it is why standing rule 7 now exists.

Known limits going in: single-symbol tools, zero threads and zero atomics
anywhere in the codebase, one validated symbol-day, and five spec lengths (`W`,
`h`, `B`, `N`, `O`) that have never met a real message.

---

## 2. What changed in the repo, and what it changes in the plan

Eight things landed that the plan has to absorb.

1. **The full-day census ran.** Phase 9's riskiest unknowns — does the file
   stream end to end, does the framing hold across every type, how big is it
   really — are retired *before* phase 9 starts. What it did not record is
   **wall-clock time**, which is the one number that would have come free. See
   9.0.
2. **The framing is verified against 268M real messages.** A whole class of
   phase-9 risk is gone: at all-symbols scale you are not discovering the wire
   format, you are discovering the book's behaviour under it.
3. **Ten spec lengths were added for unmodelled types, and three of them are
   tradability gaps** (`messages.hpp:133-143`). `h` (Operational Halt) and `W`
   (MWCB Status) both gate whether a symbol may trade, and `halt.hpp` derives
   session state from `H` alone. `B` (Broken Trade) revises a day's printed
   volume after the fact. On one symbol on one quiet day these are invisible.
   Across 8,700 symbols on two days they are reachable, and one of them can
   break the Databento comparison. **New work items in 9.6 and 9.12.**
4. **`Histogram` grew logarithmic buckets, CSV export and an SVG renderer**
   (`histogram.hpp`, `python/analysis/latency_histogram.py`), with the render
   path gated in CI. Phase 10 inherits it: the wire-to-book deliverable is a
   **distribution**, not five percentiles, and the tooling already exists.
5. **`bench/regression_check.py` gates on a ratio between two binaries built in
   the same run**, so runner speed cancels. That is the pattern phase 9's
   byte-identical regression gate should follow rather than inventing one.
6. **`scripts/update-real-numbers.py` rewrites doc tables from run artifacts and
   refuses rather than guesses.** Every results doc from here on is generated,
   not typed. Standing rule 7.
7. **`python/analysis/check_cross.py` is a third external oracle, and it is
   free.** The official closing price of a NASDAQ-listed stock *is* the closing
   cross, so it grades the `Q` path — the part of an ITCH book most likely to be
   quietly wrong, because nothing in ordinary flow exercises it. Phase 9 gets
   more out of it than phase 8 did: one run produces every symbol's crosses.
8. **`State`/`restore` snapshots landed in `ledger.hpp`, `queue_model.hpp` and
   the new `strategy_snapshot.hpp`**, with the rule that configuration is never
   restored — it is the operator's instruction, not recovered state. Phase 11's
   `InventoryStrategy` inherits that pattern and that rule.

---

## 3. The six corrections from v1.0, re-verified

Each was found by reading the source; each was re-checked against the current
tree. `book.hpp`, `order.hpp`, `pool.hpp`, `level.hpp`, `dispatch.hpp`,
`reader.hpp`, `sequencer.hpp` and `strategy.hpp` are **byte-identical** to the
versions audited, so all six stand unchanged.

1. **`BookSet` cannot be `std::vector<Book>`.** `Book` owns its own `RefMap` and
   `Pool` (`book.hpp:707-708`); `refs_capacity` defaults to `1<<20` slots
   (`book.hpp:396`) = 16 MB per book. Nine thousand books is ~144 GB of ref map
   before a single order exists. Shared storage is structural work, not a later
   optimisation.
2. **"Shared vs per-book RefMap" is not a runnable experiment** at that scale.
   Replaced in 9.9 with two that can be run and can fail.
3. **`Order::locate` is not needed for routing.** Every ITCH message carries the
   locate in its common header (`messages.hpp:83-86`). Keep the field — it fits
   in existing padding and `sizeof(Order) == 40` survives — but as a
   **cross-check**, not a router.
4. **`Side` does not support re-centring.** `set_band` returns early when banded
   (`book.hpp:168`), and the band centres on the **first order's price**, which
   at 04:00 pre-open is not a market price.
5. **`sizeof(Level)` is 32, not 40** (`level.hpp:19`).
6. **The throughput arithmetic contradicted its own premise.** Now a written
   prediction the run grades — with real inputs, below.

And three things v1.0 did not look for, all still absent from the tree and all
still required: **kernel socket drops** (10.4), the **cross-core TSC offset**
(10.0), and phase 12's **topology problem** (12.0).

---

# Phase 9 — Full day, all symbols (~4 weeks)

1.22M messages is one symbol's slice of a 268.7M-message file. Multi-symbol is
not "the same thing but bigger": the working set stops fitting in cache, memory
becomes a budgeted resource instead of a rounding error, and throughput becomes
bound by something other than the book. Each of those is the content.

### 9.0 — The measurement that is already free

`itch_census` has run the whole file. Run it once more under `time`, and record
three things in `validation/`:

- **wall-clock for the census pass.** It decompresses, frames and
  length-checks 268,744,780 messages and builds nothing. That is the **floor**
  for any end-to-end number this phase reports, measured on your hardware
  rather than inferred from a zlib throughput figure off the internet. Nothing
  else in the phase is this cheap.
- **the compressed size on disk**, next to the 8.25 GB uncompressed.
- **`--peak-orders`** — high-water mark of live orders across the feed (adds +
  replaces − deletes − full executions). This sizes the shared ref map and there
  is no honest way to get it except to count.

Then add **`--per-symbol`**: message count, first and last quoted price, price
range per locate, as JSON, committed to `validation/census-2019-12-30.json`.
Every memory number in this phase cites it.

**Check:** total from `--per-symbol` equals the type-total from the existing
path. Two ways of counting one file must agree.

### 9.1 — `Book` stops owning its storage

The structural commit; everything waits on it.

```cpp
Book(RefMap& refs, Pool& pool, uint16_t locate, int32_t tick, int32_t band_pct);
```

Single-symbol callers construct one `RefMap` and one `Pool` alongside one
`Book`, so `book_replay --symbol` behaves exactly as before. Nothing else in
this commit — no locate field, no `BookSet`, no dispatch change.

**Check:** the whole suite, plus `full-day-differential.sh` on the MSFT slice,
bit-identical. This commit must be provably invisible.

### 9.2 — Per-symbol recovery on shared storage

`clear_orders()` currently does `pool_ = Pool(); refs_ = RefMap(...)`
(`book.hpp:412`). On shared storage that wipes all 9,000 books and leaks every
other symbol's nodes — it destroys the phase-7 guarantee rather than breaking a
test. Replace with a walk: for each of this book's orders, `refs_.erase(o->ref)`
and `pool_.deallocate(o)`, then rebuild the sides.

**Check:** new test — two books on one `RefMap`/`Pool`, fill both, clear one,
assert the other's every ref still resolves and its shares are unchanged, assert
`pool.live()` fell by exactly the cleared book's order count. Then re-run the
twelve adversarial scenarios: 0 WRONG, unchanged.

### 9.3 — `Order` gains a locate, used as a cross-check

```cpp
uint8_t  side;       // 1
uint16_t locate;     // 2   <- was padding
uint8_t  _pad;       // 1
```

Routing uses the **message header's** locate. This field exists so that on every
`E`/`C`/`X`/`D`/`U` the book asserts that the order the reference resolved to
belongs to the symbol the message claims. Count mismatches into
`locate_mismatch_` and report it in the summary.

**Why it earns its byte:** a reference collision, a hash bug, or a
last-writer-wins duplicate would otherwise mutate the wrong symbol's book —
individually plausible everywhere, wrong in aggregate. This is the multi-symbol
version of "nothing tells you when you are wrong."

**Check:** `locate_mismatch == 0` across the full day. If it is not, that is the
phase's first real bug and it goes in the README.

### 9.4 — The directory and the session

Parse `R` fully into `SymbolInfo` (symbol, market category, financial status,
round lot, ETP flag) — most of `R`'s fields are currently skipped.

```cpp
class BookSet {
    RefMap                   refs_;    // one, shared, pre-sized from 9.0
    Pool                     pool_;    // one, shared
    std::vector<Book>        books_;   // indexed by locate; sized from the R pass
    std::vector<SymbolInfo>  dir_;
    SessionState             session_; // 'S' lives here, not in a Book
};
```

`S` is session-global and carries locate 0. Routing it by locate puts it in book
0 and leaves 8,999 summaries without session state — a silent corruption of
8,999 outputs, not a crash. Books construct lazily on a symbol's first message,
so locates that never trade cost a `SymbolInfo` and nothing else.

**Check:** `dir_` size equals the census `R` count; every locate seen in a
message body has a directory entry. A message for an undirectoried locate is a
framing bug — count it, do not ignore it.

### 9.5 — Dispatch routes on locate

`dispatch.hpp` keeps its shape: still the only file that knows both sides, still
mirroring `python/reference/book.py` case for case. One line at the top resolves
`BookSet::at(itch::stock_locate(p))`; `S` diverts to the session.

**Check:** the oracle's `apply()` updated in the same commit; sample differential
passes. If the two files stop being readable side by side, the commit is wrong.

### 9.6 — Tradability, at the scale where the gaps become reachable

`H` (stock trading action) is per-symbol: the `recover/halt.hpp` state machine
moves into `SymbolInfo` or a parallel array. A halt on one symbol must not gate
trading on another — today it would.

Then the three gaps `messages.hpp` now names, which one symbol on one quiet day
could not reach:

1. **`h` — Operational Halt.** A venue-level halt of a symbol, separate from
   `H`. `halt.hpp` does not see it, so a symbol can be operationally halted and
   the book will still call itself tradable. Across 8,700 symbols over two days
   this is reachable. Either model it or count occurrences and state in the
   README that tradability is derived from `H` alone — but do not leave it
   unexamined now that the run can hit it.
2. **`W` — MWCB Status.** A breach halts the entire market. Same choice, same
   requirement to state it.
3. **`B` — Broken Trade.** It busts a previously printed trade, which means a
   day's printed volume can be revised after the fact. **This one can break the
   Databento comparison in 9.12**: if a `B` lands on a symbol you are grading,
   your volume and the vendor's may legitimately differ. Count `B` per symbol,
   and if any graded symbol has one, say so next to that row rather than
   reporting a mismatch you cannot explain.

**Check:** counts for `h`, `W` and `B` reported per day, both days. A count of
zero is a result — it says the constants are still unconfirmed and why.

### 9.7 — `--all-symbols`, and the regression gate

`--symbol X` must produce **byte-identical output to the phase-8 repo**. Freeze
the current MSFT snapshot CSV and summary JSON into `validation/regression/` and
make the comparison a CI step, following `bench/regression_check.py`'s pattern —
a gate that compares two artifacts produced in the same run, not an absolute
number recorded on one machine.

`--all-symbols` writes one summary row per symbol and snapshots only for a named
subset: 9,000 books at 1 Hz would produce more output than input.

### 9.8 — The band policy

The phase-3 story — "the levels near the touch stay in L1" — cannot survive
9,000 books unmodified, and *showing that you know that* is the point.

Arithmetic for the README, with `sizeof(Level) == 32` cited: a fixed 512-level
band per side costs `9,000 × 2 × 512 × 32 ≈ 295 MB` before orders, before the
ref map. A ±2% band on a $100 stock at penny ticks is 200 levels per side; the
same percentage on a $1,000 stock is 2,000. Percentage widths mean memory you
cannot state in advance. So:

1. **Width is a tick count, not a percentage.** `--band-levels N`, default 512.
   Memory is `active_symbols × 2 × N × 32` — for the 8,892 symbols that quoted
   on 2019-12-30 that is **291 MB at N=512**, knowable before the run.

   Price-proportional width was tried against the census first and **refuted**:
   at 3% of price it costs a quarter of the memory and covers a third fewer of
   the day's adds, because the symbols that need many ticks are not the
   expensive ones. A $2.52 name with a wide day needs more slots than a $2,050
   name with a quiet one.
2. **Lazy init stays**, but centres on the first **two-sided quote**, not the
   first order. The first order of an ITCH day arrives around 04:00 and, on the
   real feed, is as likely to be a stub quote as a price anyone trades at.
   Until a band exists everything falls through to the overflow `std::map` —
   correct, just slow, and now counted.
3. **One re-centre per symbol per session**, decided once at 1,000 adds and only
   if more than 10% of them landed off-band. Re-centring rebuilds both dense
   arrays and re-indexes every resting order — price-time priority survives
   because the walk runs head-to-tail and re-push appends — so it is neither
   free nor silent: count re-centres and report how many symbols needed one.
4. **The band is a locality knob and must change no reported number.** Where a
   level lives — a dense slot or a node in the cold map — is merged away in
   price order on the way out, so a run at N=8 and a run at N=4096 must produce
   byte-identical reconstructions. That is a property, it is asserted in CI
   across a sweep of widths, and it is what makes the width a budget decision
   rather than a correctness one.

   It also came within one commit of being false. Centring on `(bid + ask) / 2`
   across a one-tick spread puts the band base half a tick off the grid, and
   `index_of()` sends anything with a remainder to overflow — so **every** price
   became off-grid and the whole book fell through to the map. Correct, and
   silently not fast. It only bit on odd spreads, so the first feed it ran on
   looked healthy.
5. **Count overflow hits per symbol; report the distribution.**

   The census ran, and it killed the version of this item that said the
   per-symbol price ranges would name the overflow symbols in advance. They
   cannot. Of the 8,892 symbols that quoted on 2019-12-30, **77.6% posted an
   order at or above $100,000 and 80.2% posted one at or below $0.01** —
   clustered on $199,999.99, $199,999.00 and $100,000.00. Those are **stub
   quotes**: orders parked where they will never fill, satisfying a two-sided
   quoting obligation. Nothing on the wire marks one; what identifies it is a
   price nothing could trade at.

   Three consequences, all of which are content:

   - A symbol's *quoted* range spans nearly the whole price axis for three
     symbols in four, so it can neither centre a band nor predict overflow. The
     census now records the *printed* range as well — `C`/`P`/`Q` carry prices
     and stubs do not trade — and that is the per-symbol scale the policy uses.
   - **Overflow is not a failure mode here; it is the design working.** Every
     symbol will hold stub quotes in its `std::map`, permanently, at any band
     width. The number to report is therefore not "how many symbols used
     overflow" (all of them will) but **what fraction of each symbol's adds
     landed off-band**, and whether any *near-touch* level ever did.
   - No generated feed in this repository produces a stub quote, so nothing
     here would ever have shown this. That paragraph belongs in
     `what-synthetic-data-hides.md`, which argued exactly this and now has a
     second worked example.

### 9.9 — Sizing, and the two experiments that replace v1.0's unrunnable one

**Sizing.** Pre-size the shared ref map to the next power of two above
`2 × peak_orders` from 9.0. State plainly that a replay knows the future and a
live system does not; name the alternative — grow online, and the tail histogram
contains multi-hundred-millisecond rehash events — and say what pre-sizing buys
you out of. Re-run the phase-4 chunk sweep at full-day scale: 4,096 → 262,144 was
tuned on one symbol's flow and the cap probably moves. Phase-4 methodology
verbatim: pin, interleave, ≥5% or it is not a result.

**Predictions written before either experiment runs:**

- **Shared pool vs per-book pools.** Prediction: shared *wins*, because
  allocation order tracks arrival order — orders that arrive together sit
  together, and a line pulled in for one message is warm for the next. The
  per-book variant is only runnable with a tiny first chunk (4,096 orders ×
  9,000 books is 1.4 GB of mostly-empty slab), and that shrinkage is part of
  what is being measured. Report it that way.
- **Ref map load factor.** Prediction: flat between 25% and 50%, because the
  probe is one cache line either way and the table is far past L2 in both cases.
  A flat result is a result and gets reported, not deleted.

### 9.10 — The framing path

Before attributing anything to zlib, measure the framing. `Reader::next()`
(`reader.hpp:46-62`) makes **two `gzread` calls and a `vector::resize` per
message** — 537 million zlib entry points over this file.

The census has since priced that whole path at **16.62 s for 268,744,780
messages: 62 ns each, 496 MB/s**. That is a good deal faster than this item
assumed when it was written, so the expected win shrinks accordingly: batching
the reads is still worth doing and still inside the rule, but it is now a
second-order optimisation against a path that is not the bottleneck. Say so
rather than quietly dropping the item — a hypothesis that measurement demoted is
worth as much as one it confirmed.

**Check:** decompress-only, parse-only and book-only reported separately, so all
three are attributable.

### 9.11 — Throughput, and a prediction the census already moved

9.0 ran, and it settles two things this document previously guessed at. Both
guesses were wrong, and in opposite directions.

**Decompression is not the bottleneck. It was never close.** The framing-only
pass — decompress, frame, length-check, build nothing — does the whole 8.25 GB
file in **16.62 s**: 496 MB/s, 16.17 M msg/s. The v1.0 draft claimed the
end-to-end run would be "decompression-bound by 3–7×". It will not be
decompression-bound at all, on this hardware. That also downgrades what phase 10's
reader thread buys: overlapping 16.6 s against a book that takes far longer is
worth having, but it is a fraction saved, not a multiple.

**The book will be far slower than the cache-hot extrapolation, and the census
accidentally measured why.** Adding `--peak-orders` took the same pass from
16.62 s to 65.58 s. That **48.96 s** bought roughly **285 M hash operations**
against a table that grows to 4.19 M slots × 16 B = 67 MB — far past any cache —
which works out at about **172 ns per operation**. It is memory-bound, and it is
the *same shape of work the book's reference map does*: same table size, same
random access by order reference, same backward-shift delete.

So the prediction, written here before the run that grades it:

> The single-symbol benchmark says 43.9 M msg/s — 22.8 ns per message — with a
> working set of a few megabytes. At full-day scale the reference map alone is
> 67 MB and the bands are hundreds of megabytes more. **Book-only wall clock is
> predicted at 60–120 s**, not the 6.1 s the cache-hot number extrapolates to
> and not the 12–18 s this document guessed one revision ago. **End-to-end will
> therefore be book-bound**, at roughly 80–140 s, with decompression a ~15%
> component rather than a 3–7× multiple.
>
> If that lands, the phase's headline result is not "we replayed a day" but
> **"the same book costs 5–20× more per message when the working set stops
> fitting in cache, and here is the counter that shows it"** — which is a phase-4
> story told at a scale where it actually bites.

Then report **two numbers, clearly labelled**: end-to-end from the `.gz`, and
book-only from pre-decompressed input. Conflating them is the kind of lie this
repo exists to not tell.

The no-third-party rule stays (no libdeflate). The legitimate fix inside the rule
is overlap — a reader thread decompressing ahead while the book consumes — which
**is phase 10**. Ship the two honest numbers and one sentence saying what that
phase can and cannot recover.

Memory instrumentation: peak RSS from `/proc/self/status` `VmHWM` at exit, plus
per-structure accounting (bands, pool, ref map, directory) so the total
decomposes and the residual is visible.

### 9.12 — Verification at scale

The oracle cannot chew 268M messages — its job description is "slow, obvious,
correct." The method changes shape; the standard does not.

1. **Sampled differential.** Seeded random selection of K symbols (K ≥ 8, always
   including MSFT and one ETF), `itch_slice` each, full oracle differential on
   each: bit-identical snapshots and summaries. Seed printed so a run
   reproduces; CI pins one, local runs rotate.
2. **Global invariants**, in one full-day pass, against an independently-run
   `itch_census`: per-type messages consumed == census counts; Σ per-symbol
   volume == total executed volume; final live orders == adds − deletes − fully
   executed; `unknown_ref == 0`; `locate_mismatch == 0`. These catch what
   sampling misses — correct per symbol, wrong in aggregate.
3. **External oracle, plural.** Databento `XNAS.ITCH` daily bars, exact on all
   five fields, for ≥5 symbols spanning the liquidity spectrum: a mega-cap, a
   mid-cap, an ETF, something illiquid, and something that halted that day.
   `validate.py`'s UTC-window rule applies to every one. Where a symbol has a
   `B` that day (9.6), annotate the row.
4. **The free oracle, at scale.** Run `check_cross.py` on every graded symbol.
   The official closing price of a NASDAQ-listed stock *is* the closing cross, it
   costs no subscription, and it grades the `Q` path — which ordinary flow never
   exercises, which is exactly why it is the part most likely to be quietly
   wrong. One all-symbols run produces thousands of crosses; report how many
   symbols produced a closing cross at all, and grade the sampled ones against
   nasdaq.com. Read those figures off the page yourself: supplying them from a
   chat transcript and then agreeing with them is a tautology with extra steps.
5. **A second day.** `01302019.NASDAQ_ITCH50.gz` is public. A different day is a
   different distribution — band policy, pool cap and halt handling all
   exercised differently — and it is the concrete argument for settling `h` and
   `W`. Same invariants, same bars.
6. **CI, which has no market data.** `make_queue_feed.py --locates N` for a
   multi-symbol synthetic feed: interleaved locates, per-symbol halts,
   cross-symbol ref uniqueness, and one deliberately high-priced symbol so the
   band policy is exercised. `fuzz_feed.py:41` already emits a second locate —
   generalise that path rather than writing a new generator.

### 9.13 — `docs/phase9-results.md`, generated

Written the same week the numbers land, and **generated from artifacts** the way
`update-real-numbers.py` generates the phase-6 tables. Extend that script or
write its sibling. No number reaches a document by being retyped.

### Done — Phase 9

- [ ] Census wall-clock, compressed size and peak live orders recorded in
      `validation/`.
- [ ] Full day, all symbols, one process: end-to-end and book-only both
      reported, bottleneck attributed by measurement.
- [ ] The written throughput prediction (6.1 s bound, 12–18 s guess) kept or
      falsified, in print.
- [ ] ≥8 sampled symbols bit-identical vs the oracle (seed printed); global
      invariants hold; `unknown_ref == 0`; `locate_mismatch == 0`.
- [ ] ≥5 symbols exact vs Databento; `check_cross.py` run on each; repeated on a
      second day.
- [ ] `h`, `W` and `B` counted on both days; tradability derivation stated.
- [ ] Peak RSS decomposed; overflow distribution and re-centre count reported;
      the band-budget paragraph written.
- [ ] `--symbol MSFT` byte-identical to the phase-8 repo (CI gate).
- [ ] Shared-pool and load-factor experiments run with predictions first.
- [ ] `docs/phase9-results.md` generated from artifacts, not typed.

**CV line unlocked:** "Replays a full NASDAQ trading day — 268.7M messages,
~8,700 symbols — through one process in X s (Y s book-only), verified by sampled
differential against an independent oracle and exact against Databento daily bars
on five symbols across two days."

---

# Phase 10 — The pipeline: wire-to-book (~5 weeks)

Every latency number in the repo today is *handler cost*: cycles around a
function call. There is no thread boundary, so there is no number resembling what
a trading system means by latency — time from bytes arriving to book updated.
This phase creates the boundary, measures across it honestly, and produces the
artifact interviews are actually about. It also closes phase 9's decompression
gap.

**Topology** — three pinned actors, three processes (processes for the replayer,
so its jitter never shares a scheduler decision with the system under test):

```
mold_replay_udp  --UDP-->  receiver thread --ring--> book thread
(paced sender)             (recvmmsg, tstamp)        (parse, apply, tstamp)
```

Needs four real cores. Confirm that before starting; on two, the numbers are
noise.

### 10.0 — `docs/phase10-methodology.md`, written first

1. **Coordinated omission.** If the sender paces off completions or stalls when
   the receiver slows, the distribution lies — slow periods generate fewer
   samples exactly when latency is worst. The sender sends on a fixed schedule
   and records *intended* send time; its own lateness is measured and reported
   separately. Name the concept; naming it is half the credit.
2. **Clock domain — the part v1.0 got wrong.** The plan of record was "one
   clock, no sync problem." The TSC is **per-core**, and this is the first time
   the repo subtracts a stamp taken on one core from a stamp taken on another.
   `tsc_is_invariant()` (`rdtsc.hpp:64`) checks `constant_tsc`/`nonstop_tsc` and
   says nothing about the offset between cores. Measure it with a ping-pong
   test and report it; if it is not negligible against the latencies being
   measured, use `CLOCK_MONOTONIC_RAW` for the cross-thread sample and keep the
   TSC intra-thread. State which one each table used.
3. **Loopback is not a network.** No NIC, no interrupt coalescing, no kernel
   bypass. Say what would change on real hardware, and that hardware
   timestamping and DPDK are out of scope by design.
4. **Pinning.** Three actors, three cores, `taskset` documented; interleaved A/B
   for any before/after claim, as `bench/compare.py` already does.

### 10.1 — `include/itchbook/pipe/spsc_ring.hpp`

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

Each decision is a comment block in the house style:

1. **Monotonic u64 indices**, masked on access. Wraparound of the index is a
   centuries-scale event at feed rates — say so and move on. Empty is
   `head == tail`, full is `head − tail == Capacity`: no wasted slot, no
   separate count.
2. **Acquire/release and nothing stronger.** Producer: `head_.store(h+1,
   release)` *after* writing the slot; consumer: `head_.load(acquire)` before
   reading it. Write down why relaxed would be wrong — the slot store must
   happen-before the publish, and only release/acquire gives you that edge — and
   why `seq_cst` buys nothing. That paragraph is a guaranteed interview question.
3. **`alignas(64)` per index** so producer and consumer never bounce a line.
   Predicted-flat candidate to actually test: 128 B padding for adjacent-line
   prefetchers.
4. **Cached counterpart indices.** The producer re-reads `tail_` only when its
   cached copy says full; ditto the consumer. Most pushes and pops become zero
   cross-core traffic. Measure on and off — this is the ring's phase-4 story.
5. **Batched publication.** The producer writes a whole `recvmmsg` batch of
   slots then publishes `head_` once: one release store per 32 messages.
6. **Slot layout: 64 bytes, and the spec table says it fits.** `spec_length()`
   (`messages.hpp:117-143`) puts the largest modelled message at 44 bytes (`P`)
   and the largest framed message at 50 (`I`). With an 8-byte arrival timestamp
   and a 2-byte length that is 54 or 60 — inside 64, with four bytes of headroom
   in the worst case. Cite the table rather than asserting "≤50 for every type
   you handle," and `static_assert` the bound so a future message type breaks the
   build instead of the run. Producer does framing only; **the consumer parses**
   — that keeps the producer's per-message work near `memcpy` so the receiver
   absorbs bursts, and keeps parse cost inside the measured region where it
   belongs.

**Check:** empty/full boundaries, wrap across the u64 mask, FIFO preserved across
2^32 pushes — the masked-index bug hunt.

### 10.2 — The TSan job

TSan and ASan cannot share a build, and CI already runs two jobs, so this is a
third: `-fsanitize=thread`, plus a stress run with randomised producer/consumer
stalls, 100M ops, sanitizer-clean. Land it *with* the ring — a lock-free
structure that has never been under TSan is an assertion, not a result.

### 10.3 — `tools/mold_replay_udp.cpp`

You have `mold_wrap` and `mold_replay`; this adds a socket and a pacer. Token
bucket over **messages**, not packets: `--rate` in msg/s, `--multiplier` over the
feed's own timestamps. The sender records its intended schedule and reports send
lateness separately.

### 10.4 — The receiver thread, and the drops that are not yours

`recvmmsg` with a batch of 32, one `cycles_begin()` per batch (amortised;
per-packet `SO_TIMESTAMPNS` is the stretch goal, and the README says which one a
table used). `SO_RCVBUF` raised, and the achieved value verified with
`getsockopt` — Linux silently caps it.

**The part v1.0 missed:** on loopback the kernel's socket buffer overflows before
your ring ever fills. Count only ring-full events and 10.5's story —
"backpressure degrades to graded feed gaps" — has a hole you did not look in: the
losses are real, upstream, and invisible. Read `/proc/net/udp` drop counts and
the `recvmmsg` error counters, and report **kernel drops and ring drops
separately** in every table. A system that knows it is drowning has to know where
the water came in.

### 10.5 — The book thread, and backpressure as a gap

Pop, parse, apply to the phase-9 `BookSet`, `cycles_end()` after apply. The
sample is `(apply_done − arrival)` into one preallocated `Histogram` per message
type plus one overall — and `Histogram` now emits **logarithmic buckets and a
CSV** that `latency_histogram.py` renders, so the deliverable is the
distribution, not five points on it. Two builds can share a p50 and a p99 and
have completely different shapes; the second is a mechanism you can go and find.
All phase-4 rules apply inside the loop: no allocation, no I/O, no printing.

Ring full → drop the **packet** and count it. A dropped MoldUDP64 packet is
precisely a sequence gap, which is precisely what `recover/gap_policy.hpp` and
the phase-7 grader exist for.

1. Ring-full → drop → sequencer sees the gap → existing policy runs (recovering
   / halted verdicts).
2. New `adversarial.py` scenario: `consumer-slow`, book thread artificially
   throttled. Existing CORRECT/SAFE/CAUTIOUS/WRONG scale; **0 WRONG** is the bar,
   same as the other twelve.
3. The kill switch gets a new input: sustained ring occupancy above a threshold.
4. **One caveat to fix or state:** the sequencer's reorder buffer is a
   `std::map` (`sequencer.hpp`), so it allocates on the producer path exactly
   when the system is stressed — a phase-4 violation at the worst possible
   moment. Preallocate it, or write the exception into the methodology doc. Do
   not let it pass unnoticed.

### 10.6 — The determinism gate

Below the knee with drops == 0, the pipeline's book must be byte-identical to
synchronous `book_replay` on the same input. The pipeline may reorder *time*,
never *effect*. CI gate.

Plus a torture test: producer pinned faster than consumer for sustained periods;
assert every message is either applied exactly once or accounted as a graded
drop. Shares conserved across the boundary.

### 10.7 — The headline artifact: the rate–latency curve

Sweep `--rate` from real-time — compute the real number from census timestamps,
do not quote a document — up through 10×, 100×, to the cliff. Plot p50/p99/p99.9
wire-to-book against offered rate with `svgchart.py`, and publish the bucket
histogram alongside it. Two required annotations: the knee where queueing delay
appears, and the **max sustainable rate** — the highest rate with zero ring-full
drops *and* zero kernel drops over a full day. That second number is the CV
number.

### 10.8 — Close phase 9's gap

The reader path becomes a thread decompressing ahead into buffers while the book
consumes — the same ring, a different slot type. Update phase 9's end-to-end
number and say which phase moved it. The 9.0 census time is the yardstick: if
overlap does not beat it, the reader thread is not doing what you think.

### Done — Phase 10

- [ ] Wire-to-book p50/p99/p99.9 **and the bucket distribution** at 1×
      real-time and at max sustainable rate, from a TSan-clean, pinned,
      methodology-documented run.
- [ ] Cross-core TSC offset measured and reported, or `CLOCK_MONOTONIC_RAW` used
      and said so.
- [ ] Rate–latency curve with knee and cliff annotated; max sustainable rate in
      msg/s; kernel drops and ring drops reported separately at every rate.
- [ ] Ring-full events land as phase-7 gaps; `consumer-slow` graded, 0 WRONG.
- [ ] Below-knee output byte-identical to synchronous replay (CI gate).
- [ ] Cached-index and batched-publish measured with predictions written first;
      one predicted-flat candidate tested and reported.
- [ ] Phase 9's decompression gap closed and the full-day number updated.

**CV line unlocked:** "Lock-free SPSC pipeline (hand-written ring,
acquire/release, cache-line-isolated indices): wire-to-book p50 X ns / p99.9 Y
ns, sustains Z M msg/s on loopback, with backpressure degrading to graded feed
gaps rather than silent loss."

---

# Phase 11 — The market-making paper (~8 weeks, interleaves with interviews)

Phase 6 measured how fills lie. This phase uses that machinery to evaluate a real
strategy from the literature — Avellaneda–Stoikov (2008) — with the honesty the
rest of the repo is built on. The deliverable is an 8–12 page paper in `docs/`,
not a P&L screenshot. The likely conclusion is that the strategy loses money on a
public feed; a paper that shows *why*, mechanically, is worth more than a fake
win, and `what-synthetic-data-hides.md` proves you already know that.

**Note the cost before starting.** Every phase up to here has a grader that is
not you: Databento, nasdaq.com, the Python oracle, ASan, TSan, the adversarial
scale, the regression gate. Phase 11 has none. That is a real step down in the
property this repo sells, and the mitigations — pre-registered predictions, an
outside reader, a Limitations section longer than the Results section — are what
stand in for an oracle. Treat them as done-conditions, not garnish.

### 11.0 — The feedback wall comes down, deliberately

`strategy.hpp` structurally denies strategies their fills and position, so one
intent stream feeds all four fill models and the P&L difference is attributable
to the model alone. **A-S cannot live behind that wall** — its reservation price
is a function of inventory `q`.

1. New interface alongside the old: `InventoryStrategy` receives a `FillEvent`
   stream and its running position. The old `Strategy` and every phase-6 result
   stay untouched.
2. The four-model band changes meaning: **four closed-loop runs**, one per fill
   model, each internally consistent — the strategy in the pessimistic lane
   *sees* pessimistic fills and quotes accordingly. The band is a band over
   worlds, not over gradings of one world. That paragraph, written correctly in
   print, is the differentiator.
3. Risk limits stay in the harness (position caps clip intents), but the strategy
   now sees position, so `strategy.hpp`'s rationale comment gets a sequel
   pointing here.
4. **It needs a `State`/`restore` pair**, following `ledger.hpp`,
   `queue_model.hpp` and `strategy_snapshot.hpp` — including their rule that
   configuration is never restored, because it is the operator's instruction and
   not recovered state. A strategy with inventory that cannot survive a restart
   breaks a property phase 7 already established.

### 11.1 — The strategy

`include/itchbook/sim/as_maker.hpp`, finite-horizon Avellaneda–Stoikov:

- Reservation price: `r(s,q,t) = s − q·γ·σ²·(T−t)`
- Total spread: `δᵃ + δᵇ = γ·σ²·(T−t) + (2/γ)·ln(1 + γ/k)`, split symmetrically
  around `r`
- Quotes clipped to the tick grid and to a max distance from mid; re-quote on a
  threshold move of `r` or on a fill, not on every tick — count re-quotes, they
  are a cost.

`σ` estimated online (rolling realised variance of mid, window a parameter). `T`
is session end. `γ` is swept, not chosen. One section if time allows: the
Guéant–Lehalle–Fernandez-Tapia inventory-bounded variant, which fixes A-S's
end-of-horizon pathology — flag the pathology either way.

### 11.2 — Calibrating λ(δ) from your own fills, the centrepiece

A-S needs `λ(δ) = A·e^(−k·δ)`. Everyone else assumes `A` and `k`; you can
*measure* them, because the MBO-resolved model knows actual fills and actual
queue-position-aware exposure.

1. Instrument the backtester to log, per resting order, `(depth δ from mid at
   each moment, exposure time at that depth, filled-or-not)`. Bucket by δ in
   ticks.
2. `λ̂(δ) = fills(δ) / exposure-time(δ)`. Fit log-linear; report `A`, `k`, `R²`,
   residual plot. If the exponential fits badly at the touch — it will, queue
   position dominates at δ=0 — **say so**: that misfit is a known A-S limitation
   and now you have the figure that shows it.
3. Calibrate on 2019-12-30, freeze, test elsewhere. Calibration and evaluation
   never share a day.
4. **The subtlety to state explicitly:** λ̂ is measured *through* a queue model,
   so `A` and `k` are conditional on it. Calibrating under the MBO-resolved lane
   and then running four lanes means three of them use parameters fitted in a
   different world. Either re-calibrate per lane — defensible, more work, and the
   band means something cleaner — or calibrate once and state the conditioning.
   Decide in writing; do not leave it implicit.

### 11.3 — Experimental design

1. **Symbols:** ≥3 across the liquidity spectrum (MSFT plus a mid-cap plus an
   ETF). Per-symbol results reported separately; no aggregate-only tables.
2. **Days:** calibrate on one, test on ≥2 others.
3. **Baseline:** the phase-6 symmetric touch-maker under the same closed-loop
   protocol. The headline question is honest and small — *does inventory-aware
   quoting lose less than naive symmetric quoting, and through which mechanism*:
   fewer toxic fills (markout improvement) or smaller inventory excursions
   (variance reduction)?
4. **Every headline number is a band** across the four models; γ-sweep as a
   figure, not cherry-picked; latency sensitivity via the existing
   `latency_sweep`. Prediction written first: A-S re-quotes often, so it should
   degrade *faster* with latency than the baseline.
5. Fees on (real NASDAQ schedule, already implemented), markouts at 100 ms / 1 s
   / 10 s, inventory path plotted, max drawdown reported.
6. **Statistical honesty:** with a handful of symbol-days there is no
   significance to claim. Frame it as a mechanism study with a day-level
   sensitivity table, and say in Limitations that N is small. Overclaiming here
   would undo the repo's whole brand.

### 11.4 — The paper

`docs/paper/as-on-itch.md` → PDF. Abstract / Data & venue / Fill models
(condensed from phase 6) / The feedback-wall change and what the band now means /
Strategy & calibration / Results / Latency sensitivity / Limitations (public
displayed feed only — no hidden liquidity, single venue, small N, loopback
latencies) / Conclusion. Every figure regenerated by one script from committed
JSON; CI runs the script, the way it already runs the histogram render.

### Done — Phase 11

- [ ] λ(δ) calibrated from own MBO fills, fit quality and touch-misfit figure
      shown; the 11.2.4 conditioning decision written down.
- [ ] A-S vs baseline: ≥3 symbols × ≥3 days (calibration day excluded) × 4
      closed-loop fill models; all headline numbers are bands.
- [ ] `InventoryStrategy` has `State`/`restore` and a restart test.
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

# Phase 12 — OUCH order entry and the closed loop (~10 weeks)

Today the matcher is a library called by a backtest. Real order flow enters an
exchange through a session protocol, and the exchange *publishes* what it did.
Your strategy speaks OUCH to your exchange, your exchange publishes ITCH, your
own phase-10 pipeline consumes it, and the strategy reacts. Every hop is code you
wrote; every hop is timestamped; the end-to-end number is **tick-to-trade**.

### 12.0 — The design decision that has to be made first

v1.0's topology — replay the recorded day's orders into the matcher as
exchange-side flow — does not work, and not for the reason it named.

Feeding a historical day's ITCH `A` messages into your matcher **does not
reproduce that day even with zero strategy orders**. Historical adds were
non-marketable against the *real* book at the instant they were entered. Entered
against *your* book — which has already diverged, because ITCH's executions are
facts you would be discarding rather than crossing events you can replay —
orders cross that never crossed. The emitted stream then diverges for reasons
that have nothing to do with your strategy, and 12.4's "backtester modelling
error" table gets dominated by that artifact.

**The topology that works is hybrid:**

- Historical flow replays as **book state**, through the phase-9 path you already
  trust. Historical orders are never matched; they are applied.
- **Only your own orders** go through the gateway and the matcher, and they match
  against that state.
- The matcher emits ITCH for **its own** mutations, which merges into the
  published stream your strategy consumes.

You lose the sentence "real flow trades against my venue." You keep an experiment
whose disagreements mean something. Write the choice into
`docs/phase12-design.md` before writing the gateway — it determines what the
gateway is for.

### 12.1 — Protocol scope

`include/itchbook/ouch/` — pick one OUCH version (4.2 is simpler; state the
choice), implement the core subset, and list deviations in the header the way
`messages.hpp` does for ITCH, including which lengths are evidence and which are
still just the spec. That convention now exists in this repository; use it.

- Inbound: Enter Order `O`, Replace `U`, Cancel `X`.
- Outbound: System Event `S`, Accepted `A`, Replaced `U`, Canceled `C`,
  Executed `E`, Rejected `J`.
- Fixed-width, alpha-padded, big-endian where the spec says so — same
  field-offset table style, same trap-hunting mindset. The token/ref distinction
  is this protocol's version of the locate trap.

**Transport:** minimal SoupBinTCP underneath — login request/accept/reject,
sequenced data packets, client and server heartbeats, end of session. Small
protocol (2-byte length + 1-byte type framing), and implementing it upgrades the
claim from "parses OUCH structs" to "implements the session layer real firms log
into." Heartbeat timeout wires into the kill switch: a dead session flattens.

### 12.2 — The gateway and the emitting matcher

1. `engine/gateway.hpp` — accepts a SoupBin session, validates OUCH (unknown
   token, bad price, wrong state → `J` with reason codes), assigns exchange-side
   refs, maintains the token↔ref map, and applies the risk layer **before** the
   matcher: `risk/kill_switch.hpp` in-line, price collar against the current
   book, max order rate, max position. A tripped switch rejects new flow and
   cancels resting orders — that behaviour is a test, not a hope.
2. **The matcher emits ITCH.** Every mutation produces its message — `A`/`F` on
   accept, `E` on execution, `X` on partial cancel, `D` on delete, `U` on
   replace, `P` for non-displayed executions if hidden slices trade — sequenced,
   timestamped, MoldUDP64-wrapped by the existing `mold` layer, published on
   UDP. Your exchange now produces the wire format it consumes, which is the
   sentence that makes the project title honest.
3. Determinism: fixed seed + fixed inbound script ⇒ byte-identical ITCH output.
   Every test below leans on it.

### 12.3 — The loop and the number

Strategy process (phase-10 receiver + ring + book on the ITCH side, SoupBin
client on the OUCH side, `InventoryStrategy` in the middle) ⇄ exchange process
(gateway + matcher + ITCH publisher), with the historical-state replayer as the
third process per 12.0.

One histogram per hop so the total decomposes — and with log buckets, a shape
per hop, not just a percentile:

- **t₀** packet arrival at the strategy socket → **t₁** book updated (phase 10's
  number)
- **t₁ → t₂** strategy decision
- **t₂ → t₃** OUCH Enter written to socket — **t₀→t₃ is tick-to-trade**, the
  headline
- **t₃ → t₄** gateway accept; **t₄ → t₅** match + ITCH publish; **t₅ → t₆** own
  fill observed back at the strategy — the full round trip.

Stacked bar at p50 and p99.9. Any hop you cannot explain, you have not finished.

### 12.4 — The validation experiment that makes it a system

**Replay-vs-live A/B.** Identical historical day, identical strategy, twice: (a)
through the phase-6/11 backtester, (b) live through the loop, with (a)'s latency
model set to the *measured* hop latencies from (b). Diff the fills. The
differences are precisely the backtester's modelling error — queue
approximation, latency-model shape, tie-breaks — and now they are enumerable.
`docs/phase12-results.md` is built around that table.

Caveats stated up front, both of them: your orders consume liquidity the
historical participants never saw (no market impact model in either lane), and
under the 12.0 hybrid those participants never react to you at all. Perfect
agreement is not expected; **explained** disagreement is the bar.

### 12.5 — Testing

1. **Cross-protocol differential**, house style: every OUCH `E` has a matching
   ITCH `E`/`P`; token-level shares conserved across both streams; the book
   built by replaying the *emitted* ITCH equals the matcher's internal book — the
   emitted feed graded by your own phase-3 machinery, so the loop grades itself.
2. `tests/fuzz/fuzz_gateway.cpp` — random valid and invalid OUCH byte streams:
   malformed lengths, unknown tokens, replace-after-fill races, cancel-of-cancel.
   Invariants: no crash under ASan/UBSan, every inbound message answered exactly
   once, ITCH/OUCH consistency holds. CI runs 1M ops.
3. SoupBin session tests: heartbeat timeout → kill switch → flatten, proven by a
   test that counts the cancels.
4. Determinism gate in CI: fixed script ⇒ byte-identical emitted ITCH.

### Done — Phase 12

- [ ] Strategy trades against your own exchange over real sockets, inside a
      replayed historical day, under the 12.0 topology.
- [ ] Tick-to-trade p50/p99.9 reported and decomposed per hop; every hop
      explained.
- [ ] Cross-protocol differential and gateway fuzz (≥1M ops) clean in CI;
      deterministic emitted-ITCH gate green.
- [ ] Kill-switch flatten-on-trip and flatten-on-session-death proven by tests.
- [ ] Replay-vs-live A/B published with every disagreement categorised.

**CV line unlocked:** "Protocol-complete exchange system: OUCH 4.2 over
SoupBinTCP into my own matching engine, which publishes ITCH 5.0 consumed by my
own feed handler — tick-to-trade measured end-to-end at p50 X µs, every hop
decomposed, backtester validated against the live loop."

---

## Calendar

| Milestone | Target | Why the date |
|---|---|---|
| Phase 9 done | late Sept 2026 | biggest CV delta per week; on the CV before applications go out |
| Phase 10 v1 (ring + a wire-to-book number) | mid Oct 2026 | "multi-threaded, real-time" must be true-on-repo before first interviews |
| Phase 10 complete | Nov 2026 | the rate–latency curve is deep-dive material during interview season |
| Phase 11 paper | Dec 2026 – Feb 2027 | talkable in later rounds; no external deadline |
| Phase 12 closed loop | Feb – May 2027 | the terminal form; new-grad cycle Sept 2027 |

Apply in September regardless of where in this document you are.

---

## Standing rules (all phases)

1. **The drill.** Twice a week minimum through interview season: re-implement one
   core structure from a blank file, unassisted, timed — `RefMap` with
   backward-shift delete, a price `Level` with intrusive unlink, the SPSC ring
   once phase 10 exists. Same for the stories: the pool-slab diagnosis, the 19.3%
   unpinned variance, the single `V` message that proved a constant, the ring's
   memory-ordering argument — out loud, whiteboard, no notes. **This is the
   schedule; the phases fill the time around it.** The repository is a promise
   about what you can do in a room with no AI in it.
2. **Prediction before measurement.** Every optimisation gets its prediction
   written down first; flat results get reported, not deleted.
3. **Two honest numbers beat one flattering one.** Decompression-bound,
   loopback, replay-only pre-sizing, kernel drops, a generated benchmark feed —
   the caveat sits in the same table as the number, the way the README's
   performance block already does it.
4. **Writing is continuous.** Each phase lands with its
   `docs/phaseN-results.md` before the next starts; the README's top block is
   updated the same day.
5. **CI is the referee.** Everything that can run without licensed data runs on
   every push; what cannot gets a committed results JSON and a script that
   re-checks it against the repo's claims.
6. **Apply on milestones, not on completion.** September applications go out with
   phase 9 on the CV. The plan continuing is not a reason to wait.
7. **No number reaches a document by being retyped.** Three rounds of auditing
   found the same defect repeatedly, and `update-real-numbers.py` exists because
   of it. Every results table in phases 9–12 is generated from artifacts, and
   `--check` runs in CI. This document's own v1.0 said "~370M messages" about a
   file that holds 268,744,780 — the rule applies to plans too.
