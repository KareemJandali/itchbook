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
  together, and a line pulled in for one message is warm for the next.

  Built: `--per-book-pools`, `--pool-first-chunk`, `--pool-max-chunk`. The
  per-book variant is only runnable with a shrunken first chunk — 4,096 orders ×
  8,906 books is 1.4 GB of mostly-empty slab — and that shrinkage is part of
  what is being measured, so it is reported alongside rather than tuned away.
  The waste is intrinsic to the *chunk*, not to the arrangement: below a
  symbol's order count, per-book is the tighter of the two, because a shared
  pool's doubling overshoots. Both directions are asserted in
  `tests/test_book_set.cpp` so the timing is read against a cost model that is
  understood rather than assumed.
- **Ref map load factor.** Prediction: flat between 25% and 50%, because the
  probe is one cache line either way and the table is far past L2 in both cases.

  **REFUTED, and it is the largest win in the phase.** 46% load → 23% load cut
  book-only time from 68.8 s to 28.4 s — **2.42×, for 67 MB**. The prediction's
  premise is the part that was wrong: at 46% load with backward-shift deletion
  it is *not* one cache line. Deletion walks to the end of its cluster, and
  cluster length grows as `1/(1−α)²` — 3.42 slots at 46%, 1.68 at 23%, a
  predicted 2.03× against a measured 2.42×. Deletes are ~140M of ~285M
  operations on this feed, which `RefMap`'s own header says is why tombstones
  were rejected. **The design comment named the dominant operation and the
  prediction costed the other one.** Going on to 11.5% load buys nothing (28.7 s),
  so the effect saturates and 4× peak is where to stop. The default moved.

- **And the band, which turned out to be the smaller knob by an order of
  magnitude.** 64× the band width (128 → 8192 slots) moves the run ~20%, and
  saturates at 2048; the reference map moves it 2.4×. The dense band is what
  phase 3 was proud of. At market scale the reference map is what costs.



All three run through `bench/full-day-sweep.py`, which applies phase 4's rules
to a run that takes a minute and a half rather than a second — interleaved,
medians, pinned where the platform has `taskset` (macOS does not, and it says
so), and refusing any difference that falls inside the within-variant spread. It
reports no timing at all until every variant has reconstructed a byte-identical
book: a knob that changes the day is not a knob.

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

### 10.4 / 10.5 — what actually happened

Built as `tools/wire_to_book.cpp`, gated by `scripts/wire-to-book-check.sh`, run
in CI under gcc, ASan/UBSan and ThreadSanitizer. Feed is
`scripts/make-synthetic-feed.py` — 252,482 messages over 64 symbols, no market
data required, which is why any of this can run on a runner at all.

**Four bugs the write-up would have shipped, and what found each.**

1. *The receiver returned from inside the batch loop on the sender's
   end-of-session packet* — which is the normal way a run ends. Everything below
   that `return` was the code that copied the sequencer's counters out. Every
   clean run would have reported **zero gaps and zero messages lost** because
   the counters were never read, not because nothing was lost. A clean sheet by
   construction, which is precisely the failure phase 7 exists to prevent. Found
   by reading the exit paths before running it; there is no test that would have
   caught it, because the number it prints is the number you expect.
2. *`hdr.message_count()` was read after `parse_header` returned false* — a live
   read of uninitialised stack deciding how much ring space to reserve.
3. *The drop decision used `writable()`*, which the ring's own header documents
   as a **lower bound** that refreshes only when it would report zero. Dropping
   on a lower bound counts drops that never happened, which would have put a
   cliff in 10.7's curve that the pipeline does not have.
4. *The sequencer can deliver more messages than the packet it was handed.* A
   packet held out of order is released when the gap in front of it closes, so
   one `on_packet` can emit a whole reorder buffer's worth. The budget guard
   added for this caught **1,065 real overruns** at `--ring-log2 12` on its first
   run — messages that would have been written past the room that was checked
   for, over slots the consumer had not finished reading.

**Loss is accounted for, and the accounting is the gate.** Three identities, all
failable, checked by the tool on every run:

- every message the sequencer delivered was staged, refused for size, or refused
  for room — there is no fourth outcome;
- the sequence cursor advanced exactly once per message delivered plus once per
  message declared lost;
- and nothing fell between the wire and those buckets.

Plus two categories that no sequencer can see and so are counted separately:
messages lost **before** it started and **after** it last advanced. A run whose
first packets are dropped starts the sequencer later, and without this those
messages vanish with nothing counting them — the drop path erasing its own
evidence.

Measured on the synthetic feed: 5,566 packets, 252,482 messages, **zero lost at
every layer** at a sustainable rate; and at `--ring-log2 10` with a 15× rate,
5,539 packets dropped, 251,479 messages declared lost, 1,003 delivered —
1,003 + 251,479 = 252,482 exactly.

**Exit codes carry the distinction the sweep needs.** `1` is a broken identity
and nothing the run printed means anything. `3` is *lossy*: the pipeline behaved
exactly as designed and the offered load exceeded what it could absorb — which
is not a failure, it is the measurement 10.7 is looking for. `4` is
*unverified*: ring drops were zero but `/proc/net/udp` is unreadable, so the rate
cannot be called sustainable. Kernel drops gate alongside ring drops, because on
loopback the socket buffer overflows first and a gate watching only the ring
would call a rate clean while thousands of datagrams went upstream.

**The gate can fail.** CI runs it twice: once expecting a clean run, and once
told to expect a clean run from a configuration that cannot deliver one. A gate
whose failure case is never exercised is a gate nobody has checked opens.

**Still open, carried into 10.6+:**

- *Item 4 above is stated, not fixed.* `sequencer.hpp`'s reorder buffer is a
  `std::map<uint64_t, std::vector<uint8_t>>`, so it allocates on the producer
  path exactly when the system is stressed — a phase-4 violation at the worst
  possible moment, and the runs above exercised it (65 gaps declared at
  `--ring-log2 12`). It is not on the measured path for a clean run, which is
  why it did not block this step, and it distorts the tail of every lossy one.
- *No latency number here is quotable.* Both threads are unpinned, the container
  has two cores, and `tsc_offset` reports UNMEASURABLE on this hardware. The tool
  prints all three caveats itself. Real figures need the pinned Linux host.
- *One stamp per `recvmmsg` batch*, not per packet. `SO_TIMESTAMPNS` remains the
  stretch goal; the output records which instrument was used.
- *The synthetic feed is not the market.* No stub quotes, no crosses, no halts,
  no NOII stream, and an invented price distribution — deliberately shaped so
  quotes cluster near the touch (6.99% of adds land off-band, 24 symbols
  re-centre) because a uniform price range sends 99% of adds to the cold
  `std::map` and would have made this measure `std::map::find`. It proves the
  pipeline conserves messages. It proves nothing about how fast a real day is.

### 10.6 — The determinism gate

Below the knee with drops == 0, the pipeline's book must be byte-identical to
synchronous `book_replay` on the same input. The pipeline may reorder *time*,
never *effect*. CI gate.

Plus a torture test: producer pinned faster than consumer for sustained periods;
assert every message is either applied exactly once or accounted as a graded
drop. Shares conserved across the boundary.

### 10.6 — what actually happened

`scripts/determinism-gate.sh`, in CI. Both halves land as stated, and the gate
earned its keep on the first run.

**Gate A: one book, six schedules.** Three ring sizes crossed with two rates.
Each clean run's `--per-symbol` output must be byte-identical to
`book_replay --all-symbols` on the same feed. Six configurations rather than one
because a single match proves the code ran; six matching across rings from 4,096
to 65,536 slots proves the answer does not depend on how full the ring got or
how often the consumer starved.

Both sides now call the *same* writer — `write_per_symbol` moved out of
`book_replay.cpp` into `include/itchbook/book/report.hpp`. A second copy of that
formatting would have made the comparison worthless in both directions: failing
on a trailing zero, or passing because both copies were wrong the same way.

A configuration that drops is above the knee on that machine, and a book built
from a feed with holes is not supposed to match one built from the whole feed.
Those runs are **skipped by name and counted**, and the gate fails if fewer than
four stayed clean — a machine that dropped everything must not report a pass
having compared nothing.

**Gate B: what was applied, against what was sent.** Under sustained overload,
`--applied-out` records the raw bytes of every message the book thread applied,
and a checker requires that stream to be an exact in-order subsequence of the
sender's feed.

The obvious version of this test cannot work, and the reason is worth keeping:
replaying the recording and diffing the books would pass even if the ring handed
the consumer a slot the producer had already overwritten, because the recording
contains the corrupted message and a synchronous replay of it reproduces the
same wrong book. Checking against the sender catches duplication, corruption and
reordering; a book-to-book diff catches none of them.

**Two bugs, and the second is the one this phase was for.**

1. *Phantom drops.* The drop decision compared `writable()`'s lower bound
   against `kBatch` instead of against what the packet needed. At `--ring-log2
   12` it refused **2,464 packets for want of room in a ring that peaked at
   1,683 of 4,096 slots** — and refused an identical 2,464 at 30,000 and at
   90,000 msg/s, because the cache's staleness is a function of how many
   messages have gone by and not of the clock. Rate-independent loss was the
   tell. This would have put a cliff in 10.7's curve that the pipeline does not
   have.
2. *A ring overrun that lost nothing.* Fixing (1) meant asking for the true free
   count before declaring a drop, computed as `capacity() - size()`. That is the
   right number obtained the wrong way. `writable()` keeps a stale copy of the
   consumer's cursor and refreshes it only when it would report zero — sound
   only while the producer publishes no more than `writable()` itself offered.
   Publishing against `size()` walked `head - cached_tail_` past capacity, and
   the next `writable()` underflowed in unsigned arithmetic to **18,446,744,073,
   709,551,612 free slots in a 1,024-slot ring**. The producer then overwrote
   slots the consumer had not read.

   Nothing was lost. Sent 252,482, applied 252,482, every identity satisfied,
   exit 0. The messages came out **reordered by exactly one lap of the ring** —
   visible in the applied stream as 24 jumps of −1023, 22 of +1025 and one of
   +2049. Every count-based check in the tool passed, because counting is not
   ordering. Only Gate B saw it.

   `publish()`'s assert catches this and is compiled out of the Release build the
   measurement runs in; the Debug build aborts with *"published more slots than
   writable() offered"*, which named the bug exactly.

   The fix belongs in the ring, not the caller: there was no way to ask for the
   true free count without breaking the cache invariant. `writable_exact()`
   refreshes the cursor and returns the truth in one call, and
   `test_spsc_ring.cpp` now has the failure reduced to eight slots — the
   `capacity() - size()` pattern reports 18 quintillion free slots on lap zero.

**Still open:** the plan's `consumer-slow` scenario for `adversarial.py` and the
kill switch's ring-occupancy input are not built. Gate B covers what that
scenario was for — conservation and ordering under sustained backpressure — but
does not grade it on the CORRECT/SAFE/CAUTIOUS/WRONG scale the other twelve use.

### 10.7 — The headline artifact: the rate–latency curve

Sweep `--rate` from real-time — compute the real number from census timestamps,
do not quote a document — up through 10×, 100×, to the cliff. Plot p50/p99/p99.9
wire-to-book against offered rate with `svgchart.py`, and publish the bucket
histogram alongside it. Two required annotations: the knee where queueing delay
appears, and the **max sustainable rate** — the highest rate with zero ring-full
drops *and* zero kernel drops over a full day. That second number is the CV
number.

### 10.7 — what actually happened

`bench/rate-sweep.py` → `validation/rate-sweep.json` →
`python/analysis/rate_latency.py` (curve + distribution) and
`scripts/phase10-report.py --check` (tables). Numbers live in
`docs/phase10-results.md` and are generated; nothing below repeats one.

**One times real time is now computed.** `itch_census` gained the session's own
clock — first timestamp, last timestamp, span, and messages per second at real
time — reported and written into both its JSON artifacts. The ladder anchors to
that. Standing rule 7 turns out to apply to a script's *inputs* as much as to a
document's outputs: a real-time constant typed into a sweep is a number that
drifts from the feed it claims to describe.

This immediately caught the synthetic feed lying about itself. Its generator
advanced the clock 1–2.5 µs per message, so 252,000 messages spanned 0.63 s and
"real time" came out at 399,000 msg/s — *above* the rate at which this pipeline
starts dropping. The first rung of the ladder would have been the cliff. The
generator now spaces arrivals as an exponential process over a stated session
length, which is both closer to the truth and the point: a market's messages
arrive in bursts, and a pipeline sized for the mean of a bursty process drops
packets at the peaks. A uniform feed hides exactly the queueing this phase
measures.

**Four decisions in the sweep, each a way of being wrong that was avoided:**

1. *Best of N, and never pooling clean runs with lossy ones.* Latency noise is
   one-sided, so best-of-N measures the pipeline rather than the machine's other
   tenants — phase 9.9's lesson. But a run that dropped half the feed did almost
   no work; letting it win its rate is how a cliff gets smoothed into a slope.
2. *The ladder extends itself.* If the top rung is still clean the sweep doubles
   and runs again. Otherwise "max sustainable rate" is a fact about how high the
   script was told to count. When the extension budget runs out with the top
   still clean, the figure is reported as a lower bound (`≥`) and the JSON
   carries `is_lower_bound: true`. The first version of this reported the top of
   its ladder as a measurement, which was the honest-looking version of making
   the number up.
3. *Offered rate is not achieved rate.* `mold_replay_udp` now reports what it
   actually managed alongside what it was asked for. They agree only while it
   keeps its schedule, and the pipeline can only have absorbed the second one.
   The curve is plotted against achieved, because plotting against offered
   stretches the x axis by exactly the amount the generator fell short — moving
   the knee to the right and flattering the pipeline.
4. *Kernel and ring drops stay separate, and UNKNOWN is not zero.* A sustainable
   rate requires zero of both.

**A third chart form.** `svg_loglog` in `svgchart.py`, because neither existing
form could draw this honestly: `svg_lines` positions x by index, which would
draw 1×→2× the same width as 400×→800× while claiming a linear axis; and a
linear y turns every percentile below the worst into a flat line on the floor,
erasing the one claim worth making — that p50 barely moves while the tail
explodes. Knee and cliff are drawn as vertical rules on the chart rather than
left to a caption.

**Two findings that survive the caveats, because they are about shape:**

- *p50 falls as the offered rate rises.* Not the pipeline getting faster: at low
  rates both threads are descheduled between messages and every message pays a
  wake-up, and the arrival stamp is per `recvmmsg` batch, which at real time is
  usually one packet. As the rate climbs the pipeline stays hot and batches
  fill. A single-threaded benchmark cannot show this at all, which is most of
  why phase 10 exists.
- *The tail leaves long before the drops do.* p99.9 climbs by orders of
  magnitude while p50 is flat and both drop counters still read zero. The knee
  is a queueing phenomenon, the cliff a capacity one, and the gap between them
  is where a system is already missing its latency budget while every counter it
  keeps looks clean.

**What this run is not.** Two cores, nothing pinned, `tsc_offset` reports the
cross-core offset UNMEASURABLE here, and the sender missed its 10 µs p99.9
schedule at *every* rate — so the sweep prints `NO RATE QUALIFIED` and says the
numbers describe the load generator. The committed artifact exists so the
machinery is exercised and the doc cannot drift; the CV number needs a pinned
Linux host. That qualification is computed and printed by the tool, not left to
whoever reads the table.

**A hypothesis that died, recorded because it will be re-derived otherwise.**
The sender is later at low rates than at high ones, which looked exactly like
the consumer's unbounded spin starving its `nanosleep` on two cores — and the
first measurement said 26 µs alone against 14.3 ms with the pipeline running,
539×. Best of five per side says 812 µs against 562 µs: indistinguishable, and
nominally better with the consumer. The original pair was one sample from each
side of a distribution spanning 40 ms. The yield was reverted; the finding is in
`docs/phase10-results.md`. Second time this phase that one sample produced a
confident wrong mechanism.

**Still open from the phase 10 done-list:** `consumer-slow` graded on the
phase-7 scale; the kill switch's ring-occupancy input; cached-index and
batched-publish measured with predictions written first.

### 10.8 — Close phase 9's gap

The reader path becomes a thread decompressing ahead into buffers while the book
consumes — the same ring, a different slot type. Update phase 9's end-to-end
number and say which phase moved it. The 9.0 census time is the yardstick: if
overlap does not beat it, the reader thread is not doing what you think.

### 10.8 — predictions, written before the measurement

Standing rule 2. Recorded here before `--reader-thread` was run once, so the
verdict below can be kept or falsified in print rather than rationalised.

**The ceiling is arithmetic, not ambition.** Decompression costs D, the book
costs B, and today they are strictly sequential: `parse()` calls `gzread`, then
the handler, then `gzread` again, on one thread. Sequential total is D + B.
Overlapped cannot beat max(D, B). So the available speedup is
(D + B) / max(D, B) — at most **2×**, and only when the halves are exactly
balanced.

**P1 — the headline.** On the real file phase 9 measured the framing-only census
at 16.62 s and the all-symbols run at 44.57 s, so D ≈ 16.6 s and B ≈ 28.0 s.
Predicted overlapped total **28–33 s**, a speedup of **1.35–1.6×**. The book is
the larger half, so the ceiling is 1.59× and the reader thread should very
nearly reach it: this producer is a single `gzread` loop with nothing to
contend on.

**P2 — on the synthetic feed in this two-core container**, where the same
arithmetic holds with different constants, predicted speedup **1.2–1.7×**.
Wider, because the sender-lateness work in 10.7 established that this
container's scheduling noise is larger than anything being measured, and
because two threads on two cores leaves nothing for the OS.

**P3 — the falsification condition.** If the overlapped time is ≥ 0.95 × (D + B),
the reader thread is not overlapping and the ring is decoration. That is the
result that would mean the design is wrong, and it is checkable.

**P4 — predicted FLAT: chunk size.** 64 KB against 256 KB against 1 MB should
show no significant difference. The ring's cost is one release store per
publish, and at 64 KB that is already one store per ~1,600 messages — a rounding
error against inflating 64 KB. Quadrupling it divides a rounding error by four.
This is the phase's predicted-flat candidate and it is expected to be flat; it
is being run precisely because "obviously flat" is what the phase 9.9 ref-map
sweep also said before it turned out to be worth 2.42×.

**P5 — which side stalls.** The producer should stall (ring full) far more often
than the consumer starves (ring empty), because B > D. If the consumer is the
one starving, the feed is decompression-bound and P1's arithmetic was built on
the wrong half.

### 10.8 — what actually happened

`include/itchbook/pipe/reader_thread.hpp`, `book_replay --reader-thread
[--reader-chunk-kb 64|256|1024]`, `bench/reader-overlap.py` →
`validation/reader-overlap.json` → `scripts/phase10-8-report.py --check`. The
graded verdicts against P1–P5 are in `docs/phase10-results.md` and are computed
from the artifact, not typed.

**The second consumer of the phase-10 ring, with a different slot type.** A chunk
of 64 KB rather than a message, because one release store per 64 KB is a
rounding error where one per 40 bytes would not be. Chunks break *between*
messages by construction — a message that does not fit is carried whole to the
next one — so the consumer never reassembles anything, which is the bug this
design exists to make impossible rather than to handle.

**Two bugs, and only one was findable by comparing books.**

1. *It hung.* The producer's fill limit was `c.len + 2 + 65535 > ChunkBytes`,
   which with the default 64 KB chunk is `c.len + 65537 > 65536` — true on the
   first iteration and every one after. It broke out before reading a byte,
   published nothing, never reached EOF, spun forever. The reservation was
   larger than the buffer it reserved from. Found by running a tool and waiting.
2. *A heap-buffer-overflow that no book comparison could see.* The carried
   message was `memcpy`'d into the next chunk with no size check. A 902-byte
   message behind a 50-byte one, with a 256-byte chunk, wrote 902 bytes into
   256. ASan caught it on the first run of the new test — and it can never fire
   on real data, because ITCH messages top out at 50 bytes and the default chunk
   is 64 KB, which is exactly why the test picks hostile chunk sizes rather than
   realistic ones. Whether a message fits is a property of the message and the
   chunk size, not of what is staged, so it is now answered on the read.

**P1 is falsified, and it is the most useful thing this step produced.** The
measured speedup went *through* the (D + B) / max(D, B) ceiling at every chunk
size. A pipeline cannot beat max(D, B), so the decomposition was at fault: the
model assumed the work is invariant under the split. Two measured reasons —
`gzread` reads an uncompressed file transparently, so the same binaries on the
same bytes isolate the book from inflate. B by subtraction overstates the book's
isolated cost (zlib's window and the ref map contend when interleaved on one
core), and the split additionally moves the per-message `gzread` and vector
resize off the consumer, which now walks a contiguous chunk. The second effect
is a cheaper inner loop rather than overlap, and it lands in the same number.
The arithmetic is still the right way to reason about what overlap can buy; it
is not a bound on what this change buys.

**P4 is undecided rather than graded, and that is deliberate.** Two runs of the
sweep measured 10.9% and 7.6% chunk-size spread against a 10% flatness bar, so a
single threshold would have decided it by coin flip. What is stable across both
runs is the *order* — bigger chunks were faster at every size — so the report
says both and claims neither more than the data supports.

**P5 is withdrawn, not graded.** It predicted the producer would stall more often
than the consumer, on the grounds that B > D. The counters cannot support that
comparison: they count *polls*, and a poll costs a different amount on each side
— the consumer's empty poll is a load and a compare, while the producer's full
poll goes through `writable()`, which refreshes the consumer's cache line
whenever it would report zero. `book_replay` printed "bottleneck:
decompression" from exactly that comparison on a feed whose own timings said the
book was the larger half. The counters now say what they are, and which half is
slower is settled by time — measure decompression alone, measure the whole run,
subtract. That is the third instrument this phase has had to demote from
authoritative to indicative.

**Tests compare the message stream, not the book.** Two paths can agree on a book
while disagreeing about which messages they saw.
`tests/test_reader_thread.cpp` checks types, bytes, order, count, and identical
failures on identical malformed input, at chunk sizes of 512 B, 1 KB and 4 KB
where almost every message straddles a boundary. It runs under ASan/UBSan and
under ThreadSanitizer, and CI additionally requires a byte-identical book at all
three production chunk sizes.

### 10.5's last two items — `consumer-slow`, and the switch's sixth input

**The kill switch gets a limit about what the system KNOWS, not what it did.**
`Trip::FeedBacklog`, on sustained ring occupancy. It belongs beside drawdown for
the same reason drawdown belongs beside the position limit: there is no order
you can decline in advance that makes it not have happened. A consumer that has
fallen behind is quoting off a book that is behind the market, and those quotes
are already gone by the time anyone notices — which is exactly the failure 10.7
measured, where the tail leaves long before a single packet is dropped and every
counter still reads clean.

The design is entirely in the word *sustained*. A deep ring is not a fault —
10.7's sweep peaked above 51,000 of 65,536 slots at rates that lost nothing —
so the limit trips on **duration above the line**, and a dip **restarts** the
clock rather than pausing it. Pausing would let a ring that is backlogged 99% of
the time never trip as long as it touched the line occasionally, which is the
system the limit exists for. Eight tests, including a burst pattern that spends
90 ms above the line fifty times over and correctly never trips.

One unit bug worth recording: the first wiring passed `bench::cycles_end()` as
the timestamp, so "sustained for 100 ms" was compared against a TSC cycle count
and meant whatever the core's frequency made it. A risk control is the last
place to infer a unit; it reads a nanosecond clock now, once per batch.

**`consumer-slow` is graded by the same grader, on the same scale.** Every other
scenario in `adversarial.py` damages a *file* and replays it. This one damages
nothing: the feed is perfect, every packet arrives, and the loss comes from the
receiver dropping whole packets because the ring is full — because the book
thread has been told to run slower than the wire. That is phase 10.5's promise
under test, and it is a promise about a code path no file scenario reaches.

**A gap has to travel through the ring like a message.** The sequencer runs on
the receiver and the book runs on the consumer, so a gap is *detected* on one
side and *acted on* on the other. Handing it over out of band — an atomic
counter, a flag — arrives at the wrong point in the stream: rebuild-forward
means discarding the book at exactly the message the gap precedes, and a flag
that overtakes the messages still in flight discards the wrong ones. So a gap
occupies a slot, in order. A gap that cannot be staged is counted separately and
**fails the run**, because that is the silent wrongness itself: the consumer
would apply everything after it having never been told anything was missing.

Two consequences worth stating. The ring now carries markers as well as
messages, so identity 1 subtracts them — an identity that failed on every lossy
run would be switched off within a week. And a new identity 1b: every gap the
sequencer declared either reached the book or was counted as unreachable.

*(Both the failing-the-run rule and identity 1b were superseded in 10.10 below,
which made an unstageable marker impossible instead of merely fatal.)*

**The throttle is a multiple of the wire's interval, not a fixed duration.** It
was absolute at first, which made the scenario silently do nothing at any rate
but the one it was tuned on — 20 µs per message against a wire delivering one
every 50 µs leaves the consumer twice as fast as the producer, so nothing filled
and `consumer-slow` reported CORRECT having dropped nothing. Caught by the
harness's own no-op rule, which every damaging scenario has had since phase 7.

Result: `pipeline-clean` CORRECT, `consumer-slow` and `consumer-stalled` SAFE,
**0 WRONG** — and the live table produces WRONG on demand when the convergence
bar is set to 1, which is what makes the SAFEs worth having. A ring too large
for the throttle to fill fails the run as a no-op rather than passing.

**One consequence found by the identities themselves.** The ring now carries
slots that are not messages, and `messages_into_ring` went on reporting the sum
— so `wire-to-book-check.sh` failed its accounting by exactly the gap count. The
field reports messages again and `slots_into_ring` reports the sum, because a
name that says "messages" and means "messages plus markers" is a trap every
future caller walks into once.

**Open, and not gold-plated here:** `clear_all_orders()` walks all 65,536 locate
slots on every rebuild, most of them null. Under sustained backpressure that is
a rebuild per gap and it measurably slows the consumer, which causes more drops,
which causes more gaps. The behaviour is correct and the cost is real; a
constructed-locate list would fix it and belongs with the next thing that needs
`BookSet` to iterate cheaply.

### 10.10 — the marker that could not be staged

Found by the determinism gate's torture leg, in CI, four commits after the code
it condemns was written and green.

**The failure.** At 3M msg/s into a 1,024-slot ring, `wire_to_book` exited 1 with
*32 gaps could not be staged, so the book applied the messages after them without
being told anything was missing*. Not a flake — the two commits since determinism
last passed touched documentation and the workflow file. The gate had simply
never been run at a load extreme enough to reach the case, and the case was the
one thing the whole design exists to prevent.

**Why it happened.** The staging budget is the room checked for *one packet's*
message count, but 10.4's item 4 is still true: a packet that closes a reorder
gap makes the sequencer release everything it was holding, so one `on_packet()`
can emit far more than it was handed. When that ran the budget out, the refusals
were *counted and carried on from* — messages as `staging_overflow`, gaps as
`gap_overflow`. Counting a hole is not announcing it. Both left the consumer
applying the messages after a hole with nothing in front of them, and the second
was fatal only *after* the run, which is a report, not a defence.

Note which of the two was already visible: `staging_overflow` had been printed
under the label *refused mid-block (no gap raised)* since 10.4, with a comment
saying in as many words that downstream gets a hole with no gap in front of it.
It was written down, and being written down passed for being handled.

**The fix: a hole is a debt, not a statistic.** Everything that vanishes after
the sequencer has advanced past it — a declared gap, a message too large for a
slot, a message the budget could not take — adds to `owed`. Nothing may be
staged until a marker carrying `owed` has been. Three properties make that total
rather than best-effort:

- once the budget is out, *nothing* more is staged from that delivery, so
  everything after the first refusal belongs to the same hole and one marker
  covers it;
- the next packet is only fed after at least one slot was proved free, so the
  debt is always payable before the next message the consumer would see;
- and end-of-session settles explicitly, waiting for a slot with a deadline,
  because at the end there is no next message to force the marker out and a hole
  one slot from the close is the same silent wrongness.

Markers therefore **coalesce**: one can carry several declared gaps plus whatever
the budget refused after them. That is what killed identity 1b — comparing marker
*count* to the sequencer's gap count is no longer meaningful. It is replaced by a
better identity, in messages rather than events:

> messages the book was told were missing + messages never announced
> = messages lost to declared gaps + oversize + refused mid-block

which compares *how much* was lost rather than *how often* loss was announced,
and how much is what decides whether the book rebuilds at the right place. Plus
a conservation check for the slots that are not messages: markers staged by the
producer must equal markers seen by the consumer.

**Measured after the fix.** One run of `wire-to-book-check.sh`'s lossy leg —
ring 2^10 at 3M msg/s, the torture configuration — dropped 5,340 packets
ring-full, declared 3 gaps covering 237,990 messages, refused 9,411 more
mid-block, and carried all 247,401 to the book in **4** markers, having made a
marker wait for a slot 11,369 times rather than discard it. `losses never
announced: 0`. The exact counts move run to run, which is the point of the
identity being checked rather than the numbers being asserted. Four consecutive
determinism-gate runs, the twelve file scenarios, and the three live-pipeline
scenarios: 0 WRONG.

**What this cost, honestly.** `staging_overflow` still means the rate was not
sustainable and still forces exit 3 — folding it into a marker makes the book
*correct* about the loss, not the pipeline *able* to carry the load. The
distinction is the whole point of exit 3 existing separately from exit 1.

**And the gate now has to prove it can still catch this.** Two additions, both
in CI. The torture leg checks the message-level identity itself, from the
artifact rather than from the tool's own say-so — a program asserting that it
noticed its own loss is one grader, and one grader is not agreement. And it
carries the no-op rule the damaging scenarios have had since phase 7: if no
marker was ever *made to wait for a slot*, the overload did not reach the path
this leg exists for and the pass would be a pass by construction — which is
precisely how the unstageable marker survived four commits of green CI.

Then a self-test. `--break-gap-markers` restores the pre-10.10 discard exactly,
and CI runs the torture leg with it and requires the gate to **fail**, matching
on *never announced*. It is a flag whose only purpose is to break the invariant,
which is a thing worth being uneasy about; the alternative was a gate nobody has
watched open.

### Done — Phase 10

- [ ] Wire-to-book p50/p99/p99.9 **and the bucket distribution** at 1×
      real-time and at max sustainable rate, from a TSan-clean, pinned,
      methodology-documented run.
      *(Pinned and TSan-clean, but NO RATE QUALIFIED: the load generator's p99.9
      lateness exceeds 10 µs at every rate under a hypervisor. Needs bare metal.)*
- [x] Cross-core TSC offset measured and reported, or `CLOCK_MONOTONIC_RAW` used
      and said so.
      *(Invariant TSC, pinning real, offset **bounded under 47 ns** — smaller
      than the ping-pong method can resolve. `validation/tsc-offset.json`.)*
- [ ] Rate–latency curve with knee and cliff annotated; max sustainable rate in
      msg/s; kernel drops and ring drops reported separately at every rate.
      *(The curve exists with knee at 25× and cliff at 50×, and the two drop
      kinds ARE reported separately — 9,151 ring-full against 0 kernel at the
      cliff. But the latency axis is disqualified with the sweep above, so the
      curve is shape without quotable numbers.)*
- [x] Ring-full events land as phase-7 gaps; `consumer-slow` graded, 0 WRONG.
- [x] Below-knee output byte-identical to synchronous replay (CI gate).
- [x] Cached-index and batched-publish measured with predictions written first;
      one predicted-flat candidate tested and reported.
      *(And a spin-starvation hypothesis that looked like 539× died under
      best-of-5 — 812 µs vs 562 µs, indistinguishable. Reverted and recorded.)*
- [x] Phase 9's decompression gap closed and the full-day number updated.
      *(P1 falsified twice: 1.78× against a 1.37× ceiling in the container,
      1.91× against 1.31× on real cores. The decomposition leaks, and better
      hardware makes the leak bigger.)*

**Five of seven.** The two open items are the same item wearing two hats: both
need a load generator that can hold a sub-10 µs schedule, which means bare metal
with isolated cores. Everything else about them — the pipeline, the pinning, the
clock, the drop accounting, the determinism gate — is done and measured.

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

### 11.0 — what actually happened

`include/itchbook/sim/inventory_strategy.hpp`,
`include/itchbook/sim/closed_loop.hpp`, `tests/test_closed_loop.cpp`. The old
`Strategy` and `Backtest` are untouched, so every phase-6 result stands.

**The wall comes down halfway, and the half that stays up is the interesting
one.** A-S needs inventory, so fills and position cross. Queue position does
not: `SimFill` carries `ahead_at_arrival` and `FillEvent` deliberately does not
copy it, because it is the estimate whose error bars are the whole subject of
phase 6 and a strategy conditioning on it would be conditioning on the thing
being measured. That omission is enforced by a `static_assert`, not a comment —
and the same assert checks the field still exists on `SimFill`, so it is testing
an omission rather than a typo.

**Position is the harness's, not the strategy's.** `PositionTracker` is fed from
the same fills the strategy is told about, so "what is my position" has exactly
one answer. A strategy that kept its own could be wrong in a way nothing would
catch — the ledger saying one thing while the quotes were priced off another.

**The band changes meaning, and this is the paragraph to get right in print.**
Phase 6's band is four *gradings of one world*: one intent stream, scored four
ways, with the difference attributable to the fill model structurally because no
feedback path exists. That is impossible once a strategy sees its fills — the
pessimistic lane carries different inventory, quotes differently, fills
differently again, and there is no longer one stream to grade. So the band
becomes four *closed-loop runs*: four worlds, each internally consistent, and
wider for a reason that is not noise — it includes the strategy's own reaction
to being filled differently. Both are legitimate and they answer different
questions; reporting one while describing the other would be the most
misleading thing this project could print, so they live in separate headers and
produce differently-labelled output.

**Two drivers that must not drift, held together by a test rather than a
comment.** `closed_loop.hpp` orchestrates the same components in the same order
as `backtest.hpp`, which is exactly the 10.6 situation: two copies of one piece
of logic either disagree on a trailing zero or agree while both being wrong. The
invariant is that a strategy *without* feedback is answering the same question in
both drivers, so both must answer identically — field for field, across all four
models. Verified to fail correctly: deleting one `markout_.observe` call makes it
report `markout_resolved 60 vs 0; markout_shares 6000 vs 0; …` and name the
model. (A first attempt at that check perturbed `event_index_` ordering and the
test passed — correctly, because nothing in that feed trips, so the reordering
changed no observable. Worth knowing that the invariant covers behaviour, not
statement order.)

`SessionClock` supplies A-S's (T − t) as progress in [0, 1], clamped rather than
extrapolated: an ITCH file genuinely contains messages before the open and after
the close, and a negative (T − t) makes the spread term imaginary. It is
configuration, so it is never restored — the same rule `ledger.hpp` and
`strategy_snapshot.hpp` follow, and it matters more here because γ, the
volatility window and the quote clamps are all instructions rather than state.

**Still open in 11.0:** the strategy-side `State`/`restore` pair exists for
`PositionTracker` and is tested, but there is no restart test driving a full
closed-loop run through a snapshot yet. That lands with the A-S strategy in 11.1,
which is the first strategy with state worth restoring.

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

### 11.1 — what actually happened

`include/itchbook/sim/as_maker.hpp`, `tests/test_as_maker.cpp` (17 tests).
Finite-horizon A-S with reservation price, closed-form spread, online σ,
tick-grid clipping, a re-quote threshold, and `State`/`restore`.

**The model is separated from the plumbing.** `as_quote()` is a pure function of
(config, mid, q, τ, σ²) and everything else — books, ticks, quote ids, re-quote
policy — sits outside it. So the algebra is checked against the paper directly,
with no book to build and no feed to replay, and a failing test is unambiguous
about whether it is disagreeing with the model or with the harness.

**Units, which is where this model is usually got quietly wrong.** The paper's
two terms are not dimensionally consistent unless inventory is a dimensionless
count: the spread term needs γ in 1/price, the reservation term needs
1/(shares·price). Implementations resolve that silently and differently, which
is one reason published A-S results are hard to compare. Here it is resolved
explicitly — dollars and seconds throughout, γ in 1/dollar in both terms, and
inventory entering as `q / inventory_unit`, a named parameter that can be swept
and reported rather than a convention buried in the arithmetic.

**And the trap that unit sloppiness sets, which caught this implementation on
its first run.** The paper's worked example uses γ = 0.1, k = 1.5. The intensity
term is `(2/γ)·ln(1 + γ/k)`, which for small γ/k is about `2/k` — so k = 1.5 per
dollar means a **total spread of about 170 ticks** on a book that quotes one
wide. Two end-to-end tests failed with zero fills, and the entire cause was
those two numbers. In equity units 1/k is the depth over which fill intensity
falls by a factor of e — a fraction of a cent to a few cents — so k belongs in
the hundreds. Defaults are now γ = 0.005 and k = 200, giving a ~3-tick spread on
a $100 name with 2% daily vol, and `test_the_default_parameters_are_in_equity_units`
asserts both that the defaults are sane *and* that the paper's values are the
170-tick trap, so the fix cannot be helpfully reverted.

**σ is estimated from mid CHANGES, not returns.** A-S models arithmetic Brownian
motion; the volatility it wants is dollars per root-second, not percent.
Sampling is on a fixed time grid rather than per message, because per-message
sampling makes the estimate a function of how busy the symbol is — tested by
feeding one estimator 500× more messages than another over the same price path
and requiring the same answer. A feed gap snaps the sample clock forward instead
of emitting catch-up samples at one price, which would collapse the variance
toward zero. A zero σ² is floored rather than allowed: it makes the spread term
vanish and the strategy quote both sides at the reservation price, which is not
conservative, it is nonsense.

**The end-of-horizon pathology is flagged, asserted, and counted.** As t → T the
inventory term decays to nothing, so the model stops skewing for inventory
exactly when it has least time to unload it — it assumes terminal inventory is
liquidated at the mid, which no desk can do. `min_time_remaining` can floor τ as
a mitigation and **defaults to zero**, so the pathology is visible rather than
papered over. A test walks τ down and requires the skew to decay strictly to
exactly zero. And the strategy counts quotes placed in the last tenth of the
session while holding inventory, so it appears as a number in the results rather
than a paragraph nobody reads. GLFT's inventory-bounded variant is the
principled fix and is not implemented.

**What the tests deliberately do NOT assert:** that a larger γ widens the
spread. It does not in general — the intensity term `(2/γ)·ln(1 + γ/k)` shrinks
with γ, so the total is non-monotonic, and the test demonstrates a case where a
larger γ gives a *narrower* spread. Asserting monotonic widening would encode a
plausible-sounding falsehood. What is asserted is that γ scales the inventory
skew linearly, and that end to end a higher γ carries less inventory.

`State`/`restore` follows the rule: what was learned travels (the σ estimator,
live quote ids, counters), what was configured does not. A test restores a
snapshot into a maker built with a *corrected* γ and requires the correction to
survive — the same protection `ledger.hpp` gives a fee schedule.

**Still open:** the restart test covers the strategy's own state round trip, not
a full closed-loop run driven through a snapshot and compared against one that
never died. That needs the `restart_check` machinery pointed at the closed-loop
driver, and it is the last piece of 11.0's carried-over item.

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

### 11.2 — what actually happened

`include/itchbook/sim/intensity.hpp`, `tools/calibrate_intensity.cpp`,
`python/analysis/intensity_fit.py`, `tests/test_intensity.cpp` (16 tests).

**λ̂(δ) = fills(δ) / exposure(δ), and the denominator is the hard part.**
Exposure is integrated per ORDER over time — two orders resting one second at
the same depth is two order-seconds — because λ is the intensity for a *single*
order. Dividing by order-seconds recovers λ rather than something proportional
to how much we happened to be quoting.

Four things are excluded, and each would bias k in a stated direction if it were
not: time when the symbol is **not tradable** (an order resting through a halt
is not being offered a fill), time when the **mid is unusable** (depth from a
mid that does not exist is not a depth), **hidden** iceberg size (in no queue,
cannot be hit), and dead entries. Each has its own test.

**Depth is integrated, not assigned at placement.** The mid moves while an order
rests, so one order migrates between buckets during its life. An estimator that
bucketed by depth-at-arrival would attribute all of an order's exposure — and
any fill — to a depth it left seconds earlier.

**Taker fills are not intensity observations.** λ(δ) is about a resting order
being hit; crossing the spread is a decision, not an arrival. Counting them
would put fills in the numerator with no exposure behind them.

**The fit is Poisson-weighted.** A bucket's fill count is a count, so
var(ln λ̂) ≈ 1/fills and the natural weight is fills. Unweighted least squares
would let a bucket holding three fills pull the slope as hard as one holding
three thousand — and the sparse buckets are always the deep ones, so the bias
has a direction. Buckets with exposure but **zero** fills cannot be logged, are
excluded, and are **counted**: dropping them silently biases the curve flat,
because the deep buckets are exactly the ones that fail to fill.

k comes out **per dollar**, matching `AsConfig::k`, so the measured value drops
straight into the strategy with no conversion anyone has to remember. Verified
by fitting the same curve generated at two different tick sizes.

**First measurement, on a generated feed** (`make_queue_feed`, 400k messages —
so these are properties of the generator, not of a market):

| model | A | k | R² | maker fills | order-seconds |
|---|---:|---:|---:|---:|---:|
| naive | 3371.6 | 38.1 | 0.532 | 5,757 | 2.4 |
| optimistic | 2930.6 | 136.9 | 0.974 | 3,081 | 30.4 |
| mbo | 2909.5 | 138.3 | 0.976 | 3,022 | 30.5 |
| pessimistic | 2883.1 | 139.6 | 0.975 | 2,988 | 30.5 |

Two things worth saying about that table. **Naive is an outlier and its own R²
says so** — 0.53 against 0.97, off the back of 2.4 order-seconds of exposure,
because a model that fills instantly leaves nothing resting to be exposed. The
estimator flags its own unreliable case without being told to. And **the
measured k ≈ 138 against the k = 200 derived from first principles in 11.1** —
same order, from two independent routes, which is the closest thing to
corroboration available before real data.

**11.2.4, the conditioning decision, decided in writing: CALIBRATE PER LANE.**

λ̂ is measured *through* a queue model, so A and k are properties of (this feed,
this strategy, *that model*). The plan offered two options: re-calibrate per
lane, or calibrate once and state the conditioning. Per lane, for a reason that
follows from 11.0 rather than from taste — once the strategy sees its fills the
four lanes are four different worlds with four different intent streams and four
genuinely different exposure denominators. Calibrating once and running four
would leave three lanes using a fill curve fitted in a world they do not live
in, which is exactly the cross-contamination the closed-loop design was built to
remove. `calibrate_intensity` therefore makes four passes over the feed and
reports four curves, and the JSON records `calibrated_per_lane: true` so a
reader never has to infer it.

The cost is honest and worth stating: four passes instead of one, and the band
now carries variation from two sources — different fills *and* different fitted
parameters. That is the price of each world being internally consistent.

**The touch misfit, which the plan predicted.** A-S assumes fill intensity
depends only on depth. At δ = 0 it depends mostly on **queue position**, which
the exponential has no way to express, so the touch bucket sits below the fitted
curve. `test_the_touch_misfit_shows_up_as_a_residual` requires a synthetic
queue-position effect to appear as a negative residual, and the tool names it in
prose when it exceeds 0.3 log-units rather than leaving it in a column.
`intensity_fit.py` draws observed against fitted rather than residuals alone —
the gap is the residual, and showing what the model believed beside what
happened is an argument where a residual plot is only a diagnostic.

**Still open:** the real calibration. These numbers come from a generated feed;
11.2.3 requires calibrating on 2019-12-30, freezing, and testing elsewhere, and
that needs the day you have plus the days you do not. The tool and the artifact
format are ready for it.

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

### 11.3 — predictions, written before the experiment exists

Standing rule 2, and committed before the harness was written so the timestamps
are not a matter of trust. Phase 10.8's P1 was falsified in the most useful
possible way; these are written in the same spirit — specific enough to be wrong.

**P1 — inventory.** A-S at moderate γ carries **smaller inventory excursions**
than the symmetric touch-maker baseline: max |position| lower by **at least
30%**. This is the model's entire purpose, so a failure here means the
implementation is wrong however well the algebra tests pass.

**P2 — the mechanism, which is the actual question.** The improvement will come
from **inventory variance reduction, not from markout improvement**. A-S skews
quotes by inventory, which mechanically shortens excursions; it does *not*
select against informed flow, and nothing in the model looks at who is trading.
So markouts at 100 ms / 1 s / 10 s should be **statistically indistinguishable**
between A-S and the baseline. If markouts improve as well, that is a real
finding that needs a mechanism, not a victory lap.

**P3 — the headline.** A-S **loses money** on a public displayed feed after
fees, in most lanes, on most symbol-days. Predicted equity per share negative in
≥ 3 of 4 lanes. A paper showing *why*, mechanically, is worth more than a fake
win — and this repo already argued that in `what-synthetic-data-hides.md`.

**P4 — latency, the plan's own prediction.** A-S re-quotes on every threshold
move of the reservation price, so it sends far more messages than a touch-maker
that only follows the touch. It should therefore **degrade faster with latency**:
across the `latency_sweep` range, A-S's equity per share should fall by a larger
fraction than the baseline's.

**P5 — the band.** The four-lane band is now over *worlds*, not over gradings of
one world (see 11.0), so it should be **wider** than the equivalent phase-6 band
for the same feed — it now includes the strategy's own reaction to being filled
differently, on top of the models' disagreement.

**P6 — the γ sweep, predicted SHAPE rather than value.** γ will move **inventory
a great deal and P&L very little** over a wide interior range: max |position|
monotonically decreasing in γ, while equity per share stays inside a band
narrower than the four-model band. If P&L turns out sharply peaked in γ, this
becomes a parameter-fitting exercise rather than a mechanism study, and the
paper has to say so.

**P7 — the falsification condition for the whole phase.** If A-S and the
baseline are indistinguishable on *every* axis — inventory, markouts, P&L,
latency sensitivity — then the closed-loop machinery is not doing what 11.0
claims and the finding is that inventory-aware quoting does not matter at this
scale. That is a publishable result and it must not be quietly reframed.

### 11.3 — what actually happened (the harness; the data is still missing)

`tools/as_experiment.cpp`, `bench/as-experiment.py`,
`python/analysis/gamma_sweep.py`, `include/itchbook/sim/inventory_strategies.hpp`.
The predictions above were committed before any of it existed.

**THREE ARMS, BECAUSE TWO CANNOT ANSWER THE QUESTION.** The plan names one
baseline — phase 6's symmetric touch-maker — and comparing A-S against it alone
controls for almost nothing: the two differ in inventory awareness *and* in
where they quote *and* in how often they re-quote. So there is a third arm:
**A-S with γ = 0**, which is the same code, the same spread formula, the same
re-quote discipline and the same quote size, with the inventory skew turned off
and nothing else changed.

`as_quote` handles γ = 0 by taking the limit rather than refusing the input —
`(2/γ)·ln(1 + γ/k) → 2/k` — so the control stays inside the same code path as
the treatment, which is the only way the two are comparable.

The decomposition that buys is the point:

> **touch → γ=0 is the SPREAD choice. γ=0 → A-S is the SKEW.**

On a generated feed (so: a property of the generator, not a market), the mbo
lane at γ = 0.005 gave equity per share −18,401 → +3,210 → +8,810 across the
three arms, with inventory σ 12,082 → **72,983** → **220.6**. The spread choice
alone made inventory *worse*; the skew is what fixed it, cutting σ by 331×. A
two-arm comparison would have shown "A-S has better inventory" and attributed
the whole thing to inventory awareness. With the measured k = 138 instead of the
assumed 200 the P&L signs move, and the skew *costs* about 7,000 µ$/share while
removing ~28,000 shares of inventory σ — a risk/return trade rather than a free
lunch, which is exactly the mechanism the paper has to describe.

**Per symbol-day, never pooled.** The C++ tool does one symbol-day and records
which. The Python driver keeps every row and emits three tables — evaluation
days only, the mechanism decomposition, and a day-level sensitivity table that
shows the **spread** across days rather than a mean. No mean anywhere: with a
handful of symbol-days there is no significance to claim, and an average invites
exactly the claim the data cannot support.

**Calibration and evaluation cannot share a day, and it is enforced rather than
documented.** `--calibration-day` is excluded from the evaluation tables, and a
run whose every feed is on the calibration day **exits non-zero** rather than
warning. CI checks that refusal, because evaluating on the day you fitted k on
is the most comfortable mistake available here.

**The γ sweep is a figure with two panels and two different axes.** Inventory is
log–log — it spans decades across the sweep, which is the model working, and on
a linear axis everything but the smallest γ is a flat line on the floor. P&L is
linear **with zero in range**, because P&L is read by its sign and an axis that
crops zero makes a small profit look like a large one. One line per fill model,
because since 11.0 every headline number is a band and a single line would be
picking a world.

**A caveat found by running it.** The γ = 0 control's spread is `2/k`, so it
depends on the k it is given. With the assumed k = 200 that is half a cent — the
control quotes *inside* the touch and fills 58,684 times, which makes it a
liquidity taker in disguise rather than a control. The experiment must therefore
be run with the k **measured** in 11.2 for that symbol-day and lane, not with a
placeholder. `--k` exists for that and the driver's help says so.

**What is still missing is the data, and it is the whole of 11.3's substance.**
The harness runs, the design is enforced, the figures render — on generated
feeds. Every prediction P1–P7 remains ungraded because grading them needs ≥3
symbols across the liquidity spectrum and ≥3 days with calibration excluded. One
`wget` per day from `emi.nasdaq.com/ITCH/` unblocks it.

### 11.4 — The paper

`docs/paper/as-on-itch.md` → PDF. Abstract / Data & venue / Fill models
(condensed from phase 6) / The feedback-wall change and what the band now means /
Strategy & calibration / Results / Latency sensitivity / Limitations (public
displayed feed only — no hidden liquidity, single venue, small N, loopback
latencies) / Conclusion. Every figure regenerated by one script from committed
JSON; CI runs the script, the way it already runs the histogram render.

### 11.5 — what the first real data changed before it produced a single result

Four days of NASDAQ ITCH were selected and one downloaded — 30 Aug 2019, the
calibration day. Before any experiment ran, the data corrected two things.

**The reconstruction held on unseen data.** 310,317,357 messages, 8,841 books,
1.07 billion shares: zero unknown references, zero locate mismatches, zero
undirectoried messages, in 50 s at 546 MB. Every message that did not reach a
book is accounted for by type — 3,943,840 unmodelled (I, J, L, V, Y) plus 8,841
`R`, 8,849 `H` and 6 `S`, which sums exactly to the 3,961,536 gap. Derived
prices matched reality across twenty symbols (MSFT $137.69, GOOG $1,188.94,
AAPL $208.51), which is an oracle nobody had to write.

**Symbol selection was nearly made on the wrong statistic.** Ranking by `adds`
puts MVO and XLSR mid-table — and they had **53 and 4 trades** in the whole
session, against 920 and 5,840 adds per trade. That is market makers quoting
into a vacuum. λ(δ) is fitted from *maker fills*, so those symbols cannot
calibrate anything; ranked by trades instead, 1,869 of 8,841 symbols clear 1,000
trades, which is where the thin arm actually lives.

**The axis that matters is spread in ticks, and it was measured rather than
assumed.** Price was the proxy; the measurement was the referee:

| symbol | mean spread | at 1 tick | touch qty (bid × ask) |
|---|---:|---:|---:|
| GOOG | 60.7 ticks | 0.0% | 72 × 44 |
| STOR | 1.6 ticks | 71.8% | 338 × 442 |
| MSFT | 1.4 ticks | 65.2% | 445 × 430 |
| AMD | 1.0 ticks | 99.2% | 3,417 × 3,831 |

61× in spread and 62× in queue depth, running opposite. The proxy was right
about GOOG and AMD and **wrong about STOR**, which was picked expecting
thin → wide and is in fact the most tick-pinned at the mode (71.8% at one tick,
more than MSFT) because at $37.75 a penny is 2.6 bp against MSFT's 0.7. Its
higher mean is a fat tail — 10.9% of the session at ≥3 ticks against MSFT's
2.5% — which makes it the only candidate whose spread regime *changes* during
the day, and a better choice than the reasoning that selected it.

**And that measurement exposed a collapse that would have ruined the
experiment.** `tools/as_experiment.cpp` held `k` as a single scalar applied to
every fill model and every symbol; `bench/as-experiment.py` passed one `--k` for
the whole matrix. That discards both dimensions the paper claims:

- **Per lane** — the §6.1 conditioning. On one synthetic feed the fitted k
  already ran 43.7 (naive) to 143.3 (pessimistic), a 3.28× spread, and the
  calibrator's own output has been printing *"the spread between these k values
  is the cost of assuming one"* since 11.2. Nothing consumed it.
- **Per symbol** — indefensible across a 61× spread range, and invisible until
  a real day was measured, because on generated feeds every symbol looks alike.

The tool was not dishonest about it: its banner printed **"assumed k"**. The
calibrator existed, the experiment existed, and the wire between them was never
run with real values because there was no data to run it with.

k now reaches the experiment **from the committed calibration artifact, never
from a flag typed by hand** — standing rule 7 applied to an input parameter
rather than a document. The driver refuses a symbol with no calibration, a
calibration whose recorded day is one being evaluated, a lane whose intensity
could not be fitted, and a partial lane set; `--allow-assumed-k` remains for
smoke runs and stamps every artifact `k_source: assumed-scalar`. The calibration
artifact now records its own symbol and day, so the day check is a check rather
than a convention about filenames. All five refusals run in CI, each verified to
fire before being committed.

### A portability bug that eleven phases of green CI could not see

Found after 11.4 was merged, by running `scripts/verify-local.sh` on a Mac for
the first time. Both builds failed on one line in `tools/wire_to_book.cpp`, live
since phase 10.6:

```
std::printf("%-32s %14" PRIu64 "\n", "books built", books.books());
```

`BookSet::books()` returns `size_t`. On Linux x86-64 `size_t` and `uint64_t` are
both `unsigned long` — the *same type* — so this is not a tolerated mistake
there, it is correct, and `-Wformat` has nothing to report. On macOS `uint64_t`
is `unsigned long long` and `size_t` stays `unsigned long`, so the same line is
a `-Werror=format` build failure. Every CI run from 10.6 through 11.4 was green
while the repository did not compile on macOS at all.

The lesson is a level up from the one phase 10 already recorded. That one was
*two compilers, because neither front end sees everything*. This one is **two
data models, because neither platform sees everything** — gcc and clang agree
with each other on Linux precisely because the types are identical there, so no
number of Linux compilers could have found it.

Three things changed, one fix and two gates:

- The line uses `%zu`.
- **`macos-build-and-test` is now a CI job.** Build and unit tests only; the
  soaks and the UDP work stay on Linux rather than being paid for twice. What it
  buys is the front end over a different data model.
- A grep in that job names the specific mistake — a `size_t` accessor formatted
  with `PRIu64` — rather than leaving the next person to decode a format
  warning. It was checked against the reintroduced bug before being committed,
  because a guard that has never fired is a guard nobody has tested.

An audit of the other 197 `PRI*64` uses in the tree found no second instance:
the four other call sites that mix the two either cast explicitly or already use
`%zu`.

### 11.4 — what actually happened

`docs/paper/as-on-itch.md`, and four scripts that decide what it is allowed to
say: `scripts/paper-report.py`, `scripts/paper-figures.sh`,
`scripts/paper-html.py`, `scripts/paper-pdf.sh`.

**The paper has no results, and that is the state it is designed to hold.** The
evaluation needs data this repository does not have. The interesting question
was therefore not how to write the results section but how to keep an unfinished
paper from *looking* finished, and the answer is that the results sections do
not exist in the Markdown at all — they are spliced between markers by
`paper-report.py`, which emits, when `validation/as-experiment.json` is absent,
an explicit block naming what is missing and the exact command that would
produce it. There is no path by which a number reaches that page without a
committed artifact behind it, and no way to leave the section blank and have it
read as an oversight.

The seven §11.3 predictions are graded **by computation** in §7.5 — each against
a bar transcribed once into the script — for the reason phase 9's report script
demonstrated: it printed "kept" from a string literal for a while after the
numbers had moved outside the predicted range. A hand-written verdict is not a
graded prediction; it is a claim about one.

Three properties are enforced rather than intended:

- **Figures have provenance, and an orphan is a failure.** A committed chart
  whose artifact is not committed is a picture nothing can reproduce or refute,
  so `paper-figures.sh --check` fails on it rather than skipping. Each figure's
  artifact, that artifact's SHA-256, and the exact command land in
  `docs/figures/paper/manifest.json`. The plotter picks the symbol-day and the
  caption reports it — one rule, recorded once, read back by the report
  generator, instead of two rules that agree until they don't.
- **The generator is exercised with data on every push.** Today only the refusal
  branch runs, so CI builds synthetic feeds, runs the whole experiment through
  them into a scratch copy of the tree, and asserts the paper comes back with a
  graded seven-row table, a scope warning, and three inlined figures. A results
  generator first executed on the day the real data arrives is a results
  generator nobody has tested.
- **The build is stdlib-only up to the last hop.** Markdown → HTML is
  `paper-html.py`, checked by CI like every other generated document. HTML → PDF
  needs a print engine and is therefore the one step that depends on what is
  installed; the PDF is a build output and is not committed, because a binary
  whose bytes depend on a font cache is not something a diff can referee.

One bug worth recording, because it is the same shape as the one that took CI
red four times earlier in this phase: the first version of `paper-figures.sh`
piped its manifest entries into `python3 - <<'PY'`, where the heredoc *is*
stdin — so the pipe was silently discarded, and the script wrote an empty
manifest beside three freshly drawn figures while reporting "no figures, no
manifest — consistent". A checker that cannot fail is worse than no checker, and
it took generating figures with real content to notice.

### Done — Phase 11

- [ ] λ(δ) calibrated from own MBO fills, fit quality and touch-misfit figure
      shown; the 11.2.4 conditioning decision written down.
      *(The decision is written down — paper §6.1, and `calibrated_per_lane` in
      the artifact. The calibration itself has not been run on real data.)*
- [ ] A-S vs baseline: ≥3 symbols × ≥3 days (calibration day excluded) × 4
      closed-loop fill models; all headline numbers are bands.
      *(Harness complete and exercised in CI on synthetic feeds; the paper
      refuses to print a results table until the artifact exists.)*
- [x] `InventoryStrategy` has `State`/`restore` and a restart test.
- [ ] The band-over-worlds methodology paragraph written and reviewed by someone
      who did not write it.
      *(Written — paper §4. Not reviewed. This is the item most likely to be
      wrong in a way its author cannot see, and it cannot be self-cleared.)*
- [ ] Latency-degradation prediction written before the sweep, kept or falsified
      in print.
      *(P4 is committed and the grader is built: it reads every
      `validation/as-experiment*.json`, keys them by `latency_ns`, and reports
      "not evaluated" while there is only one. No sweep has been run.)*
- [x] Paper PDF builds from committed sources; one script regenerates every
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

**Written.** [`docs/phase12-design.md`](phase12-design.md) settles it, and
settles three consequences the paragraph above does not reach:

- **Adds are state, executions are events.** "Historical flow replays as book
  state" taken literally makes strategy orders *unfillable* — the aggressor that
  would have hit your maker appears in the feed only as its consequence on
  someone else's order. So `A`/`F`/`U`/`X`/`D` are applied, while `E`/`C`/`P` are
  replayed as synthesised aggressors through the matcher, which walk the queue
  and may take your shares first. An add is a fact about the book; an execution
  is a fact about a trade, and only the second is a crossing event you are
  entitled to replay as one.
- **One book, reference space partitioned.** Historical and strategy orders rest
  in one queue or queue position means nothing. Strategy references take the high
  half of the 64-bit space and the replayer *asserts* no historical reference
  enters it — a collision is a silent book corruption, phase 12's version of the
  locate trap.
- **Two clocks.** The market runs in replay time (drives message timestamps and
  the strategy's `T − t`); tick-to-trade is wall clock. Feeding wall clock to an
  A-S horizon during a 50× replay changes the strategy rather than the load.

Five predictions are written there before any code. **P1 is the one that decides
the phase:** with zero strategy orders, the book built from the *emitted* feed
must be byte-identical to the phase-9 book from the *original* feed. It is
testable on data already in hand, before OUCH or SoupBinTCP exist, and it is
built first.

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
