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

- [x] Census wall-clock, compressed size and peak live orders recorded in
      `validation/`.
      *(`census-2019-12-30.json`: 3.52 GB on disk, 8.25 GB uncompressed,
      268,744,780 messages, **65.94 s** for the framing + live-order pass
      (16.51 s framing-only, `census-2019-12-30-framing.json`), 1,924,078 peak
      live orders. The 44.57 s in the next item is the all-symbols replay, which
      is a different pass and a different artifact.)*
- [x] Full day, all symbols, one process: end-to-end and book-only both
      reported, bottleneck attributed by measurement.
      *(44.57 s end-to-end, 16.51 s framing-only, **28.06 s the book's own
      cost**. Decompression is 37% of the run, and the plan's claim that the run
      would be decompression-bound by 3–7× is refuted in print.)*
- [x] The written throughput prediction (6.1 s bound, 12–18 s guess) kept or
      falsified, in print.
      *(Graded in `docs/phase9-results.md`, with an "inside the prediction"
      column. **This line names the two guesses 9.11 superseded**, not the
      prediction it committed: that was 60–120 s book-only, 80–140 s end-to-end.
      It is **falsified** for the configuration that shipped — 28.1 s and 44.6 s
      — and kept for the 4.19M-slot map it was written against. Both rows are in
      the table; the load-factor sweep below is what moved it.)*
- [x] ≥8 sampled symbols bit-identical vs the oracle (seed printed); global
      invariants hold; `unknown_ref == 0`; `locate_mismatch == 0`.
      *(**Done, and it failed the first time it ran.**
      `scripts/sampled-differential.py` pins MSFT and an ETF, draws the rest
      from the symbols that quoted with a seed it prints, slices each and
      requires both comparisons — snapshot CSV and daily summary — to agree.
      **Seed 20191230, K=10: 10 of 10 bit-identical across 612,280 snapshot rows
      and 220 summary fields**, artifact in
      `validation/sampled-differential-2019-12-30.json`.

      What it caught: **three of the ten disagreed on `vwap`** — SEEL, ESBA and
      ABIO. Not the book. `volume` and `notional`, the exact integers vwap is
      derived from, matched on all ten, as did all 61,228 snapshot rows each.
      The C++ printed `%.10f` while the oracle writes Python's shortest
      round-tripping repr, and ten decimals does not always round-trip a double.
      It does for MSFT's vwap, and it does for the generated feed the regression
      gate is pinned to — so **the single-symbol differential and the regression
      gate were both blind to this by coincidence of the data they had**, which
      is the whole argument for a seed instead of a choice. Fixed by emitting
      `%.17g`; the gate re-recorded to no change, which is itself the evidence.

      One defect in the harness, found by testing the harness: its first run
      reported eight symbols "bit-identical" over **zero** snapshot rows,
      because the interval exceeded the synthetic session. A comparison of two
      empty files passes. It now reports VOID and fails.

      The rest of the item was already true: `unknown_refs`
      and `locate_mismatch` are **0 on both days**. **All fifteen global
      invariants hold and all fifteen run, on both days**: `full-day-check.py`
      on the committed census, run and per-symbol CSV prints "OK: 15 global
      invariants hold across 268,744,780 messages and 8,906 symbols" and exits 0
      with no skips, and the same on 2019-08-30 across 310,317,357 messages and
      8,841 symbols. (The two that need the census type histogram have had it
      since `981dc61`, "The census, with the type histogram"; the three
      `'h'`/`'W'`/`'B'` cross-checks came with the 9.6 field work, and
      2019-08-30 got its first census at the same time.)
      ~~**The ≥8-symbol differential was never run.** 9.12 changed the shape of
      verification to global invariants and this check did not survive the
      change — which is a substitution, not the item, and is recorded as one
      here rather than only in the results doc.~~ **It has now been run**, and
      the substitution is retired: the global invariants and the sampled
      differential both hold, which is what 9.12 asked for in the first place.)*
- [x] ≥5 symbols exact vs Databento; `check_cross.py` run on each; repeated on a
      second day.
      *(**All three clauses closed, and closing the third replaced an oracle
      that had been measuring the wrong universe.**

      **≥5 symbols, repeated on a second day: DONE.** Ten symbol-days are graded
      against Databento `XNAS.ITCH` `ohlcv-1d` and all five fields are exact on
      every one — five symbols on 2019-08-30 (ALLE, AQB, ELTK, MSFT, QQQ) and
      five on 2019-12-30 (ALLE, AQB, MKD, MSFT, QQQ), each oracle response
      committed beside its row as `databento-<SYM>-<DATE>.json` so the verdict
      re-checks offline. The basket spans the spectrum the item asks for: a
      mega-cap, an ETF, a mid-cap, something barely traded, and something that
      genuinely halted — MKD with 16 halts in December, ELTK with 3 in August,
      MKD having not yet listed then. **The whole basket cost $0.0000156.** This
      item was carried for the life of the project as "the one with a cash
      cost"; the cash was a rounding error and what actually blocked it was the
      reconstructions not existing yet.

      **`check_cross.py` on each: 10 graded, 0 failed.** 13 auction prices
      exact and 7 absences agreed, against **Databento's `statistics` schema on
      `XNAS.ITCH`** — the venue's own published `OPENING_PRICE` and
      `CLOSE_PRICE`, committed per symbol-day as
      `databento-stats-<SYM>-<DATE>.json`. The absences are the half no
      consolidated source can check: ALLE is NYSE-listed and holds no NASDAQ
      auction, so we reconstruct no cross print and the venue publishes
      `UNDEF_PRICE`, and those agreeing is a result rather than a gap. AQB has
      an opening cross and no closing one on both days; MKD, 16 halts, has a
      closing cross and no opening one. The `statistics` basket cost
      **$0.0000143**, an order of magnitude less than the `trades` schema that
      would have required inferring the cross from a day of prints.

      **It got there through a failure that turned out to be the oracle's.**
      The prices were read by hand off a consolidated quote history, which is
      right for the close — for a NASDAQ-listed stock the official closing price
      *is* the closing cross — and wrong for the open, where a consolidated bar
      reports the first print across every US venue. That coincided with the
      opening cross four times out of five and failed on MSFT 2019-08-30: ours
      139.1000 against a published 139.15. The volumes gave it away — Yahoo
      reports 23,940,100 shares where `XNAS.ITCH` carries 9,674,474, and Yahoo's
      high is *lower* than ours, which two views of one tape cannot do.
      `check_cross.py` had already rejected consolidated VOLUME for exactly that
      reason and then used a consolidated bar's open anyway.

      **The venue's own figure is 139.1000.** The reconstruction was right, the
      oracle was not, and the four rows that passed had been passing by luck —
      which is worth more than the one that failed. The failure was not reasoned
      away on that argument, either: the row stayed FAIL until a same-universe
      source said otherwise, because a failure explained away by the person who
      wrote the thing is what this basket exists to prevent.

      **Preparing them found a bug that would have failed the grading**, which
      is the argument for the outside oracle made concrete. An auction that did
      not happen still arrives as a Cross Trade with `shares == 0` and
      `price == 0`, and both implementations folded it into the daily
      statistics: `low` dragged to zero, `open`/`close` set to zero, and a trade
      counted that never occurred. On 2019-12-30 it corrupted **`low` on 6,468
      of 8,906 symbols, `close` on 5,903 and `open` on 4,976**. Volume, notional
      and adds were untouched — an empty cross carries no shares — which is why
      every global invariant passed over it. It survived because MSFT, the one
      symbol ever graded externally, has real auctions at both ends, and because
      the generated feed the regression gate is pinned to has no empty crosses
      at all. `itch_census` had the same defect in its own counting: it reported
      **8,906 of 8,906 symbols with a closing cross**, where the truth is
      **2,537** (and 3,086 with an opening cross; 2,545 and 2,873 on
      2019-08-30). That is the denominator 9.12's fourth item asks for, and it
      was meaningless until now. Fixed in all three, with regression tests in
      `test_book.cpp` and `tests/test_reference.py` that fail without the fix,
      and both days' artifacts regenerated.

      **The Databento clause is now closed: ten symbol-days, two days, all five
      fields exact on every one.** Table in `validation/README.md`, each oracle
      response committed beside its row as `databento-<SYM>-<DATE>.json` so the
      verdict replays offline and the fetch is bought once. Total spend for the
      basket: **$0.0000156**. This item was carried for the life of the project
      as the one with a cash cost; the cash was a rounding error, and what
      actually blocked it was that the reconstructions did not exist.

      The external oracle then did the job it exists for. ALLE's low reads
      **95.6800 and matches Databento to the cent**; before the empty-cross fix
      it read **0.0000**. MSFT's row did not move at all, which is exactly why
      one graded mega-cap could never have found it.

      **The clause still open is `check_cross.py` on each.** It needs the
      official opening and closing prices, and this item's own instruction says
      to read those off the page rather than take them from a transcript. Two
      further things are now known about that clause and were not before: the
      reconstructed auction prices are finally trustworthy — `none` where there
      was genuinely no auction, rather than a fabricated `0.0000` — and **ALLE
      cannot be graded by it at all**, because it is NYSE-listed and has no
      NASDAQ auction on either day. So the gradable set is MSFT and QQQ on both
      days, ELTK, MKD's close and AQB's open. The denominator that item asks
      for is also real now: **2,537 symbols produced a closing cross on
      2019-12-30**, not the 8,906 the census used to report.)*
- [x] `h`, `W` and `B` counted on both days; tradability derivation stated.
      *(**Done — all three, both days, and checked rather than merely counted.**
      `h` and `B` are **zero** on both days —
      `operational_halts` and `broken_trades` in `all-symbols-2019-08-30.json` and
      `all-symbols-2019-12-30.json`. (The first of those was committed as
      `census-2019-08-30.json`, which it never was: it carries `feed`, `adds`
      and `band_levels`, so it is a `book_replay --all-symbols` run. Renamed, and
      the name `census-2019-08-30.json` now holds an actual census.) A zero is the result 9.6 asked for: the
      constants stay unconfirmed against real bytes and the count says so.
      `W` is acted on — `dispatch.hpp` routes it to `set_mwcb_breached()` and
      `tradable()` reads it — and `book_replay` prints it in the summary as
      `MWCB level breached ('W')`, ~~but it is **not written to the JSON**, so it
      is not per-day verifiable from `validation/` the way the other two are —
      one field, not an experiment.~~
      **The field now exists.** `write_all_json` emits `mwcb_level_breached`
      (empty string when the session never breached, not a `0` that would be a
      fourth level nobody defined) and `mwcb_events`, with a `mwcb_events()`
      accessor on `BookSet` behind it. More than a field, though, because
      *verifiable* was the word the item used: all three are now
      **cross-checked against the census type histogram** by
      `full-day-check.py`. `'W'` and `'B'` are exact equalities — dispatch calls
      `set_mwcb_breached()` and `note_broken_trade()` once per message — and
      `'h'` is a **bound**, because a repeated halt on an already-halted symbol
      is not a second entry. That is three more checks, so a fresh run now
      reports **fifteen** global invariants rather than twelve, and all three
      were confirmed able to fail before being trusted. Two things it does not
      buy: the committed run artifacts predate the fields, so the checks SKIP
      against them and the file exits 2 — the twelve that always passed still
      pass — and 2019-08-30 has no type histogram to check against anyway.
      **Both days were then re-run and the artifacts committed**, so the
      qualification above is spent: `full-day-check.py` reports **15 of 15 on
      2019-12-30 and 15 of 15 on 2019-08-30**, exit 0, with nothing skipped.
      `W` is zero on both days **on the evidence** — neither census records a
      single `'W'`, and 2019-08-30's census is new, because that day had never
      had one, which is precisely why its `h`/`W`/`B` had nothing to be checked
      against before. The derivation is stated in
      `book_set.hpp::tradable`, which gates on `H`, the operational halt and the
      MWCB level. All three stay **false** for `modelled()` deliberately: that
      predicate is the contract `python/reference/book.py` mirrors, and none of
      the three is a book mutation. **The `messages.hpp` block that lists them
      went stale when 9.6 landed — it still described `h` and `W` as
      unreflected — and was corrected in `a05de8c`**, which files all three
      under ACTED ON and carries the MWCB-treated-as-permanent limitation
      across.)*
- [x] Peak RSS decomposed; overflow distribution and re-centre count reported;
      the band-budget paragraph written.
      *(Three of four. RSS decomposes to **92.5%** — bands 291.8 MB, reference
      map 134.2 MB, pool 83.7 MB, residual 41.3 MB. Re-centres are reported per
      day (4,319 and 3,988) and per symbol. The band-budget paragraph is "The
      band is where the design actually failed", and it carries the phase's real
      finding: 30% of symbols had at least half their adds off-band.
      ~~**The overflow distribution is not reported** — overflow maps appear only
      inside the residual row, which is the one part of the memory story that is
      an aggregate hiding a distribution, exactly the thing this phase criticised
      the 13% off-band figure for being.~~
      **Going to report it turned up why it never was: the metric could not
      answer the question.** `overflow_levels()` is the map's size *right now*;
      the session ends flat, and `pop()` erases each overflow level as it
      empties to keep the map cold — so a completed day reports an empty map for
      every symbol however hard overflow was worked in between. Both committed
      days say exactly that: **8,906 and 8,841 symbols, zero overflow levels
      each, against 18.7M and 14.0M off-band adds.** A figure that is zero by
      construction cannot decompose peak RSS, which is itself a high-water mark,
      and a distribution over it would have been a column of zeroes presented as
      a finding. So `Side` keeps a high-water counter now — taken in the
      overflow branch of `push()` and nowhere else, so the dense path pays
      nothing, and deliberately *not* reset by `clear_levels()`, because a
      re-centre empties the map and the peak is a fact about the session.
      `peak_overflow_levels` reaches the per-symbol CSV and the run JSON, with
      `overflow_bytes_per_level` beside it so the report prices the maps out of
      the residual row instead of typing a node size; the summary row that used
      to count a structural zero prints the peak with the terminal count beneath
      it; and `test_peak_overflow_outlives_the_levels_it_counted` fails if the
      peak ever stops surviving what it counted. The published table is **empty
      rather than estimated** ~~and names the re-run that fills it — the same
      re-run as the item above~~ **until the re-run filled it**, which it now
      has: **8,892 of 8,906 symbols on 2019-12-30 and 8,826 of 8,841 on
      2019-08-30 used overflow**, median peak 36 levels, p99 475, max 5,593
      (AMZN) — and **zero at the close on both days**, which is the prediction
      the counter was built on, confirmed at full scale.
      One caution carried into the results doc rather than celebrated: pricing
      the peak at 72 B a node puts overflow at 41.2 MB against a residual that
      was 41.3 MB, and the decomposition therefore reads 100.0%. **That is too
      neat to accept.** The row is an upper bound twice over — the two sides'
      peaks are summed though they need not coincide, and peak RSS is its own
      high-water mark — and 8,906 books, a directory and the binary are not
      0.1 MB between them. The real figure sits under the bound and the other
      terms cover the rest; the doc says so instead of claiming every byte.)*
- [x] `--symbol MSFT` byte-identical to the phase-8 repo (CI gate).
      *(`validation/regression/`, gated by `scripts/regression-gate.sh` on every
      push — so all of the above was bought without moving the single-symbol
      path.)*
- [x] Shared-pool and load-factor experiments run with predictions first.
      *(`validation/sweep-pool.json` and `sweep-load.json`. The load factor was
      predicted to measure **flat** and measured **2.42× book-only** (68.8 s to
      28.4 s) — the largest single speedup in the phase, and a falsified
      prediction that took the end-to-end run from 87 s to 44.6 s.)*
- [x] `docs/phase9-results.md` generated from artifacts, not typed.
      *(`scripts/phase9-report.py`, `--check`ed in CI.)*

**Nine of ten**, and the tenth is two-thirds done. `W` per day and the overflow
distribution are closed: the
fields were added, the cross-checks and the high-water counter that make them
mean anything were added with them, and **both days were re-run and their
artifacts committed** — which is what actually closed the boxes, because an
artifact that *would* carry a number is not an artifact that *does*. The
global-invariant count went twelve to fifteen and both days now report 15 of 15
from committed bytes.

**The ≥8-symbol oracle differential is closed too**, and it failed on its first
run — see its entry. The one item still open is the Databento row, and only one
of its three clauses remains: `check_cross.py` on each symbol, which needs
official auction prices read off a page rather than taken from a transcript.
The Databento grading itself is done — ten symbol-days, two days, all five
fields exact — and it cost **$0.0000156**, so "the only item costing money" was
never really about the money.

Both of the closed items found real bugs, which is the argument for having run
them rather than reasoning about them. The differential found a `vwap` the two
implementations printed differently, agreeing only by coincidence on the single
symbol it had always been run on. Preparing the Databento basket found an
auction that did not happen being counted as a trade at price zero, which had
corrupted `low` on 6,468 of 8,906 symbols in both implementations at once —
invisible to every internal check, because both were wrong the same way.

Worth recording because it was the expensive part and none of it was the book:
the re-run reproduced every deterministic field on both days and **did not
reproduce the wall clock** — 52.49 s against 44.57 s. Chasing it cost more than
the fields did and ruled out, by measurement, the code (`dfe2837`'s own binary
reports the same 52.49 s today), the power state, the build flags and I/O. What
is left is that the machine is busier than it was. The recorded figures are kept
and the attempt is published beside them in
[`validation/timing-reproduction-2026-08-24.json`](../validation/timing-reproduction-2026-08-24.json),
because a performance number should measure the program rather than what else
was running — the same reason every sweep in this phase reports the minimum of
its samples.

**CV line — not yet earned.** The line below is written for the finished phase
and **two of its clauses are outstanding**: there is no sampled differential
against the oracle at full-day scale, and Databento has graded one symbol on one
day, not five across two. Do not put it on a CV until items 4 and 5 are ticked.

> "Replays a full NASDAQ trading day — 268.7M messages, ~8,700 symbols —
> through one process in 44.6 s (28.1 s book-only), verified by sampled
> differential against an independent oracle and exact against Databento daily
> bars on five symbols across two days."

What **is** earned today, and is the stronger claim anyway because every number
in it is committed and CI-checked: "Reconstructs every one of 8,906 securities
from a full NASDAQ TotalView-ITCH day — 268.7M messages, 8.25 GB — in one
process in 44.6 s at 551 MB, with zero unknown order references and zero locate
mismatches across the entire feed, cross-checked by fifteen global invariants
against a bookless census pass on two separate trading days, and exact against a
vendor daily bar on one symbol."

Fifteen, and both days carry it from committed artifacts — `full-day-check.py`
exits 0 with nothing skipped on 2019-12-30 and on 2019-08-30. The wall clock in
that sentence is the recorded one and its reproduction attempt is published; see
the note above.

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

**The part v1.0 missed:** on loopback the kernel's socket buffer can overflow
before your ring ever fills. Count only ring-full events and 10.5's story —
"backpressure degrades to graded feed gaps" — has a hole you did not look in: the
losses are real, upstream, and invisible. Read `/proc/net/udp` drop counts and
the `recvmmsg` error counters, and report **kernel drops and ring drops
separately** in every table. A system that knows it is drowning has to know where
the water came in.

*Measured, and it went the other way — which is the same argument from the far
side. At `--rcvbuf-mb 16` (32 MB granted) against a 65,536-slot ring, the sweep
records tens of thousands of ring-full drops and ZERO kernel drops: the book is
the bottleneck and the receiver drains the socket as fast as `recvmmsg` will go.
Shrink the buffer to 2,304 bytes and the order reverses, which is what the
negative self-test in `scripts/wire-to-book-check.sh` relies on. Which one
overflows first is a property of the sizing, not of loopback — and that is
exactly why both are reported at every rate rather than one being assumed.*

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

- [x] Wire-to-book p50/p99/p99.9 **and the bucket distribution** at 1×
      real-time and at max sustainable rate, from a TSan-clean, pinned,
      methodology-documented run.
      *(Taken on bare metal — an i7-11700K booted from a live USB into Ubuntu
      26.04, pipeline on CPUs 7/6/5, three distinct physical cores, run as root
      so SCHED_FIFO and `mlockall` were granted rather than denied.*

      *At 1× real time (83,849 msg/s): p50 **6,189 ns**, p99 **22,696 ns**,
      p99.9 **27,885 ns**. At max sustainable (2,096,222 msg/s): p50 **5,290 ns**,
      p99 **1,081,195 ns**, p99.9 **4,606,539 ns**. The bucket distribution is in
      `validation/rate-sweep.json` under `max_sustainable.buckets`, 54 buckets in
      raw TSC cycles — divide by `cycles_per_ns`, which is what `buckets_unit`
      in the same file exists to tell you.*

      *The entrance exam is what makes those quotable. The sender alone, against
      a port nothing is listening on, held p99.9 lateness to **46 ns** at
      200,000 msg/s and **810 ns** at 1× — against a 10,000 ns bar, and against
      106,002 ns and 294,423 ns for the same binary on the same silicon under
      WSL2. `validation/sender-qualification/baremetal-*.json`.*

      *One caveat that belongs with the number rather than after it: p99.9 climbs
      to 1.5 ms at 5× and 3.4 ms at 10× on rows that are otherwise clean, with
      peak ring occupancy of 2,318 and 5,107 of 65,536 slots. There is not enough
      queue at those rates for queueing to explain it. p50 and p99 are stable
      across a 25-fold rate change; the far tail below the knee is not yet
      accounted for.)*
- [x] Cross-core TSC offset measured and reported, or `CLOCK_MONOTONIC_RAW` used
      and said so.
      *(Invariant TSC, pinning real, offset **bounded** — the estimate comes back
      smaller than the ping-pong method can resolve, which is what healthy
      hardware gives. The bound moves run to run (47, 48, 58 ns on three
      successive runs), so the figure lives in `validation/tsc-offset.json` and
      in the generated table, not in this sentence.)*
- [x] Rate–latency curve with knee and cliff annotated; max sustainable rate in
      msg/s; kernel drops and ring drops reported separately at every rate.
      *(**Knee at 25×** — 2,096,222 msg/s, where p99 leaves a 22,696 ns baseline
      and reaches 1,081,195 ns. **Cliff at 50×** — 4,192,445 msg/s, 10,516
      ring-full drops against 0 kernel drops. **Max sustainable 2,096,222 msg/s**,
      and `is_lower_bound` is false, so the ladder actually found the cliff
      rather than running out of rungs. 7 of 9 rates had the sender holding its
      schedule; the two that did not are the top rungs, where it achieved 21.6M
      of 33.5M offered.*

      *The two drop kinds are reported separately at every rate, and the kernel
      column is now a measurement rather than a constant — see below.*

      *Those annotations moved, and not because the pipeline did. Three defects
      in the harness were found and fixed first: the sweep started the generator
      at `bind()`, 91–102 ms before the book thread existed; the driver passed
      `--rt-priority 80` into RT bandwidth throttling, which parks a saturating
      real-time thread for 50 ms and was manufacturing the largest number in the
      table; and the run was taken at `--messages 200000`, small enough that a
      65,536-slot ring absorbed a burst and reported it as a sustained rate. The
      kernel-drop column was worse than wrong: it could not be non-zero. See
      below.*

      *The two headline deltas — 25× sender p99 lateness 26,603,394 → 116,005 ns,
      and max sustainable 3,356,615 → 838,489 msg/s — come from a single
      before/after pair in which the feed size, the rung ladder, the scheduling
      class and `mlockall` all changed at once. Neither number isolates its own
      fix, and the "before" artifact is not committed. They are the size of the
      combined correction, not a controlled measurement of one cause.*

      *One thing that did NOT move: under WSL2, the same sweep pinned to 13/14/15
      with the book and the sender on two hyperthreads of core 7 produced the
      identical knee, cliff and max sustainable rate as the distinct-core run on
      11/13/15. SMT placement was not what limited that host; the hypervisor was.
      `tools/cpu_jitter` puts a number on it — an idle pinned CPU there was
      off-CPU longer than 10 µs about 1,343 times a second with an 11 ms worst
      case, against 30 times a second and 43 µs on bare metal, and **zero** gaps
      over 100 µs where the guest had ~90 a second.)*
- [x] Ring-full events land as phase-7 gaps; `consumer-slow` graded, 0 WRONG.
- [x] Below-knee output byte-identical to synchronous replay (CI gate).
- [x] Cached-index and batched-publish measured with predictions written first;
      one predicted-flat candidate tested and reported.
      *(And a spin-starvation hypothesis that looked like 539× died under
      best-of-5 — 812 µs vs 562 µs, indistinguishable. Reverted and recorded.)*
- [x] Phase 9's decompression gap closed and the full-day number updated.
      *(P1 falsified four times: 1.78× against a 1.37× ceiling in the container,
      1.91× against 1.31× on real cores, 1.98× against 1.21× on the WSL2 re-run
      at the full feed, and **1.980× against a 1.471× ceiling on bare metal**. The decomposition leaks, and better
      hardware makes the leak bigger. A run at `--messages 200000` KEPT P1
      instead — D 0.03 s, B 0.04 s, T_seq 0.07 s, every timing quantised to a
      hundredth of a second, so the ceiling was arithmetic on three rounded
      centiseconds. Not re-derived as a finding, and not asserted as an
      artefact either: the report script now DERIVES the verdict from
      `exceeds_model_ceiling` and the per-chunk fractions, so whichever way a
      run comes out, the sentence follows it.)*

**Seven of seven.** The two items that stayed open for three runs were the same
item wearing two hats, and both closed the moment the load generator could hold
a sub-10 µs schedule — which took bare metal, and *not* merely isolated cores.
`isolcpus` was available under WSL2 and measurably bought nothing; the isolated
CPUs came out marginally worse than the general pool. What the guest could not
do was hold a CPU at all: `tools/cpu_jitter` recorded a pinned thread on an idle
CPU off-CPU for longer than 10 µs **1,343 times a second**, worst gap 11.09 ms,
with all four CPUs stalling together in the same ~300 ms window while the kernel
credited them with 19,998.7 ms of 20,000 and reported 5 involuntary context
switches. The same tool on bare metal: **30 a second, worst 43 µs, and not one
gap over 100 µs.** `validation/cpu-jitter.json` and
`validation/cpu-jitter-baremetal.json`.

Three defects in the harness had to be fixed before any of it counted, and one
of them had never produced a measurement at all: `kernel_drops` was a constant,
not a number, for every run this tool ever took — `/proc/net/udp` was read after
`close(fd)`, and the drops column is padded with trailing spaces so the parse
returned zero even when the row was there. Either bug alone was sufficient.
There is now a negative self-test that forces kernel drops with a 2,304-byte
receive buffer and fails if the counter stays at zero.

**One of those, the drop accounting, was not.** `wire_to_book` read
`/proc/net/udp` *after* `close(fd)`, and a UDP socket leaves that file the
instant it closes; `kernel_drops()` then answered the missing row with 0 rather
than its "cannot tell you" sentinel. Fixing the ordering was not enough — the
drops column is padded with trailing spaces, so `strrchr(line, ' ')` landed on
the padding and `strtoull` parsed whitespace. Either bug alone was sufficient.
So `"kernel_drops": 0` was a **constant, not a measurement**, for every run this
tool ever took: exit 4 was unreachable, the kernel half of the LOSSY gate was
dead code, and `max_sustainable` rested on a check that could not fail. Both are
fixed and there is now a negative self-test that forces kernel drops with a
2,304-byte receive buffer and fails if the counter stays at zero — which is how
the second bug was found, on the test's first run.

**CV line, filled:**

> Lock-free SPSC pipeline (hand-written ring, acquire/release,
> cache-line-isolated indices): wire-to-book p50 holds at 5.3–6.2 µs from
> real-time rates to 25× real time, p99.9 27.9 µs at real time; sustains
> 2.1 M msg/s with zero drops, backpressure degrading to graded feed gaps
> rather than silent loss.

*Every figure carries the rate it was measured at, and that is the point rather
than a caveat. The sentence originally drafted for this slot wanted one pair of
latencies and one throughput, and the run does not offer them at the same
operating point:*

| | p50 | p99 | p99.9 | rate |
|---|---:|---:|---:|---:|
| 1× real time | 6,189 ns | 22,696 ns | 27,885 ns | 83,849 msg/s |
| max sustainable | 5,290 ns | 1,081,195 ns | 4,606,539 ns | 2,096,222 msg/s |

*Taking p50 and p99.9 from the first row and the rate from the second would read
as one claim and be two: the 27.9 µs tail and the 2.1 M msg/s never happened
together, and at 2.1 M the p99.9 is 4.6 ms — **165× larger**. It is the first
thing a reader who knows this material would ask about, and a line that invites
that question has spent its credibility before the answer.*

*So the line leads instead with what is true across the whole range, which is
also the harder result: **the median moves by under a microsecond while the
offered rate moves by 25×** — 6,189 ns at 1×, 5,272 ns at 10×, 5,290 ns at 25×.
A good number at one operating point is a measurement; a flat one across
twenty-five is a property.*

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

### 11.6 — the conclusion, and what it took to be allowed to write one

The paper had results in §7 and stopped at "Reproducing". Three sections were
still hand-written and three sections were now wrong: the status banner said
*results pending data* above a graded seven-row table, and §9 *What remains*
opened with "the evaluation is not run". That is the same defect as phase 10's
"UNMEASURABLE on this hardware" sitting above a measured bound — prose that was
true when typed and false afterwards, with nothing able to tell.

So the banner, the abstract's finding sentence and the whole of §8 are generated
now, from the same artifact §7's tables come from, and the paper is §1–§7,
§8 Conclusion, §9 Limitations, §10 Reproducing.

**The finding.** Over 3 symbols × 3 days × 4 lanes, A-S captures **less**
half-spread per share than a naive touch maker in **36 of 36** cells and goes
outright negative in 16, against 0 of 36 for the baseline; one-second markout is
worse in 32 of 36. The inventory prediction survives — median max|q| is 75%
below the baseline's — so A-S buys inventory control with adverse selection,
which is the reverse of the pre-registered mechanism.

**Two things the three-arm design bought, and a two-arm design would have
lost.** The `as-gamma0` control separates the *spread choice* from the *skew*,
and they point opposite ways: the skew is a real and isolated win (with it, A-S
holds less inventory than the naive maker on 3 of 3 symbols; **without** it, A-S
holds *more* on 3 of 3), while the spread choice is where every dollar of the
edge goes, and it is the larger of the two on 3 of 3.

**The mechanism, and it is about measurement rather than about A-S.** The
inventory-free floor `(1/γ)·ln(1 + γ/k)` is 8.12 ticks on GOOG and **0.31 and
0.49 ticks** on MSFT and STOR — below half a tick, below the smallest increment
the venue can quote. The sign of A-S's captured edge follows that one test on
3 of 3 symbols. An implementation that *assumes* k never finds out that its own
spread formula is asking for a price the venue does not have.

**A defect the generator found in itself.** The first draft computed every count
correctly and then stated the *direction* in a fixed string. Run against CI's
synthetic feed — where A-S happens to beat the baseline — the page read
"0 of 4 cells" immediately above "it is a different market, made worse", and
"1 symbols". A generated conclusion that can only conclude one thing is a
hand-written conclusion with extra steps. Every directional sentence now branches
on a computed `most()`, the tick claim only firms up once **both** sides of the
line have been observed (`0 < wide_floor < symbols`), and CI asserts the
conclusion exists in one branch and refuses in the other.

### 11.7 — P4 is graded on equity, and equity is mostly the stock

P4 was pre-registered as *A-S degrades faster with latency, measured on
equity per share*. It stays graded that way — a bar that moves once the data is
in is not a bar, and standing rule 2 is the whole reason the predictions were
written before the harness existed.

But §7.1 already says `equity = edge + drift − fees` and that drift is the term
belonging to the stock rather than the strategy. On GOOG drift is a median 86%
of equity and ranges over −153,774 to 2,450,158 µ$ per share across days — a
spread **70× as wide** as edge's own 139,804 to 177,184. A fractional *equity*
change between two latencies is therefore substantially a change in what the
stock did to two different inventory paths.

So §7.5 now carries the identical test on **edge** beside P4's verdict, labelled
as not the pre-registered bar, with a table including `as-gamma0` so the latency
damage can be attributed to the spread choice or the skew without an inventory
effect in the way. Reporting only equity would be quoting the noisier of two
instruments because it was named first; reporting only edge would be moving the
bar. Both, and the reader can see why they differ. CI asserts the companion
appears **with** the verdict and never instead of it.

**Measured, at 0 ns against 500,000 ns.** P4 is **kept** on equity at 19/36
(53%) — a bare majority. On edge it is 25/36, and the two agree. The interesting
part is per symbol: **GOOG 12/12, MSFT 9/12, STOR 4/12**. A-S degrades faster on
two symbols and *slower* on the third. Half a millisecond also takes the naive
touch maker's own edge negative on both penny-wide names — 3,942 → −833 on MSFT,
3,506 → −1,693 on STOR — which is §8.3's tick argument again from the other side:
when the whole edge is a fraction of a tick, there is nothing left to lose.

**And the first version of that table pooled.** It reported 36 cells across
three symbols and said A-S degrades faster, full stop — hiding STOR's reversal
entirely. §7 states in as many words that results are reported *per symbol-day
and never pooled*, because with this many symbol-days an average invites exactly
the claim the data cannot support. The rule was written down, applied to every
other table in the section, and broken by the next table added to it. Per symbol
now, with the per-symbol counts printed.

**One more, about reading a table rather than computing one.** The level columns
are medians of levels and the change column is a median of per-cell ratios, and
the two do not compose: pooled, `as-gamma0` read 445 → −1,562 beside "−66%",
which is 451% by the levels. Both numbers were right and the pair was
unreadable. The column is labelled for what it is, with a note on why they can
diverge where edge crosses zero.

**And a transcribed number found inside the generator.** §7.1's sentence
carried a hand-typed *"drift was 94% of equity"* — in the one file whose entire
job is to stop numbers being typed. By the time anyone checked, the artifact
said 86%. Standing rule 7 applies to the generator exactly as hard as to the
document: a literal in there is indistinguishable, in the output, from a
measurement.

Two bugs in the replacement, both caught by running it rather than reading it:

- Picking the illustrative symbol by `|drift| / |equity|` selects whichever
  symbol's equity landed nearest zero. STOR scored 129% because its drift and
  edge partly cancel, and the sentence named the least illustrative case in the
  sample. A ratio with a small denominator is not evidence of a large numerator.
  It selects on **drift range over edge range** now, which is the claim being
  made.
- Rendering µ$ as dollars at two decimals reported STOR's edge as *"between
  $0.00 and $0.00"* — a true statement that destroys the number it is about.
  The sentence stays in µ$ per share, as the rest of the section does.

### Done — Phase 11

- [x] λ(δ) calibrated from own MBO fills, fit quality and touch-misfit figure
      shown; the 11.2.4 conditioning decision written down.
      *(Four symbols calibrated on 2019-08-30 — `validation/intensity-*.json`.
      Fitted k spans 11.9 to 483.9 across symbols and lanes. AMD's fit fails on
      3 of 4 lanes and the artifact says so rather than emitting a number.)*
- [x] A-S vs baseline: ≥3 symbols × ≥3 days (calibration day excluded) × 4
      closed-loop fill models; all headline numbers are bands.
      *(GOOG, MSFT, STOR over 2019-10-30, 2019-12-30, 2020-01-30, calibrated on
      2019-08-30 — `validation/as-experiment.json`, 288 runs. Tables and the
      seven graded predictions are generated from it.)*
- [x] `InventoryStrategy` has `State`/`restore` and a restart test.
- [ ] The band-over-worlds methodology paragraph written and reviewed by someone
      who did not write it.
      *(Written — paper §4. Not reviewed. This is the item most likely to be
      wrong in a way its author cannot see, and it cannot be self-cleared.)*
- [x] Latency-degradation prediction written before the sweep, kept or falsified
      in print.
      *(**Two real latencies are committed** — `validation/as-experiment.json`
      at 0 ns and `validation/as-experiment-500us.json` at 500,000 ns, the same
      nine evaluation symbol-days in both — three symbols over three days, which
      is where the 36 cells come from at four lanes each. (Each artifact also
      carries the 2019-08-30 calibration day, so twelve `(symbol, day)` pairs
      are present and nine are graded.) **P4 is graded `kept`**, 19 of 36 cells, in
      paper §7.5; §8.5 records it as done. The grading branch had already run in
      CI on two synthetic latencies before the second real artifact landed,
      which is why the code that read it was not being executed for the first
      time on the data it had to grade.
      §7.5 also carries P4's edge companion, and the reason it exists is worth
      keeping: P4's pre-registered bar is **equity**, and a fractional equity
      change between two latencies is substantially a fact about what the stock
      did to two different inventory paths. The identical test on **captured
      edge** — the part latency can actually act on — is reported beside it. It
      agrees with the verdict and does not replace it; the bar does not move
      once the data is in.)*
- [x] Paper PDF builds from committed sources; one script regenerates every
      figure; CI runs it.

**Five of six.** The one open item is the only thing in this document that
cannot be closed by writing code: §4 needs a reader who did not write it.

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

**The order below changed when this phase was split, and the reason is P1.**
v1.0 ran protocol → gateway → loop → validation, with the emitting matcher in
the middle and its P1 check as one item inside it. But P1 needs no OUCH, no
session layer and no sockets — only data already in hand — and it is the single
result that can invalidate the topology. Everything protocol-shaped is therefore
sequenced *after* it. The old "12.5 — Testing" section is gone as a section: its
four items were gates on four different steps, and a testing section at the end
of a ten-week phase is the structure that lets them slip.

| step | what it closes | weeks |
|---|---|---|
| 12.1 | the replayer splits adds from executions | 1.5 |
| 12.2 | **P1**, and the determinism gate | 1.5 |
| 12.3 | OUCH 4.2 | 1.0 |
| 12.4 | SoupBinTCP | 0.5 |
| 12.5 | gateway and risk | 1.5 |
| 12.6 | fuzz and the cross-protocol differential | 1.0 |
| 12.7 | the closed loop | 1.5 |
| 12.8 | tick-to-trade, decomposed | 1.0 |
| 12.9 | replay-vs-live A/B and the results doc | 1.0 |

Ten and a half against a ~10-week envelope. 12.8 is the one that cannot absorb
the overrun, because it needs a bare-metal window rather than more hours.

### 12.1 — The replayer splits: adds are state, executions are events

12.0's decision, made mechanical. `A`/`F`/`U`/`X`/`D` are **applied** to the
book through the phase-9 path you already trust. `E`/`C`/`P` are **replayed as
synthesised aggressors** through the matcher, walking the queue and taking your
shares if they are in front of them. An add is a fact about the book; an
execution is a fact about a trade, and only the second is a crossing event you
are entitled to replay as one.

Reference space is partitioned in this step rather than later: strategy
references take the high half of the 64-bit space, and the replayer **asserts**
no historical reference enters it. A collision is a silent book corruption —
phase 12's version of the locate trap, and it wants the same treatment the
locate trap got, which is an assert that runs in CI rather than a comment.

**Done when:** a full phase-9 day replays with zero strategy orders and the
resulting book equals the phase-9 book at every message; the partition assert is
armed and never fires. No OUCH, no sockets, no emission yet — this step is
gradeable entirely against machinery that already exists.

### 12.1 — what actually happened

Built as [`include/itchbook/replay/split.hpp`](../include/itchbook/replay/split.hpp),
gated by [`scripts/split-replay-gate.sh`](../scripts/split-replay-gate.sh) and
`tools/split_replay_gate.cpp`, with `tests/test_split_replay.cpp` in ctest.

**The gate passes on the full day.** 268,744,780 messages of
`12302019.NASDAQ_ITCH50.gz`, 5,722,824 executions replayed as crossing events,
**0 per-message divergences, 0 end-state differences, per-symbol CSV
byte-identical, 0 partition violations** — 5m18s, 1.09 GB peak RSS, recorded in
`validation/split-replay-2019-12-30.json`. The comparison is genuinely
per-message and not end-of-day: both paths run over one feed in one process and
the book the current message touched is compared after every message, which is
O(1) because a message can only change the book it is routed to. A divergence
that appears at message 40,000,000 and heals by the close is invisible to a
comparison of final states, and a healing divergence is the one a strategy would
have traded through.

**Two of the design document's classifications are wrong, and the feed says so.**
`docs/phase12-design.md` §3 groups `E`, `C` and `P` together as "replayed as an
aggressor". Asking where in its own queue each executed order was sitting at the
instant it traded:

| | at front of its level | at best price, behind others | worse price than best |
|---|---|---|---|
| `E` (5,722,824) | 99.82% | 10,565 (0.185%) | **0** |
| `C` (99,917) | 17.6% | **82,376 (82.4%)** | 273 (0.27%) |

`C` is Order Executed With Price — an execution that prints *away* from the
resting price — and 82.4% of them name an order a mean 4,501 shares deep in its
own queue. Those trades did not respect displayed price-time priority; they are
price improvement and non-displayed interaction, and every one of the 273
away-from-best cases happened while the book was crossed. Replaying a `C` as a
queue-walking aggressor would hand a resting strategy order fills it would never
have received, which is the optimistic-fill error phases 6 and 11 exist to
prevent. `C` is applied as state. 445× is not a rounding difference.

`P` is applied as state for a narrower and harder reason: it consumed
NON-DISPLAYED liquidity, and `Book::trade()` correspondingly moves counters
without touching a resting order. There is no displayed liquidity for it to
walk. Whether a displayed strategy quote should have been filled ahead of the
hidden order it actually traded against is a modelling question about display
priority, and it is left to 12.7 rather than guessed at here.

**`E` never trades through the displayed book.** Zero of 5,722,824 executions
named an order resting at a worse price than the best on its side. That is the
fact that makes a targeted aggressor safe, and it was worth measuring rather
than assuming: the obvious construction — a plain marketable order priced at the
resting order's price — would walk from the front of the best level and eat
whatever it found there.

**What the aggressor actually does.** It takes strategy shares ahead of the
named order, then the named order, and **skips historical orders ahead of it**.
The feed is ground truth about *which* historical order traded, so a historical
order resting in front of the named one demonstrably did not trade here,
whatever our reconstruction's queue says; consuming it would invent a fill that
history contradicts. It happens 88,133 times in a day and is counted rather than
swallowed. With zero strategy orders the walk finds nothing and the path reduces
to exactly the phase-9 mutation, so the gate passes by construction rather than
by luck.

**One new line in `Book`.** `take()` is `execute()` minus `record_trade()` — the
book mutation is identical and the tape statistics are not — so an aggressor
routed through the engine leaves volume, notional, trades and OHLC at zero.
`Book::note_feed_trade()` gives the other half back, because the replayer splits
one historical execution across two consumers while the tape still saw ONE
print: recording per fill would increment `trades_` twice and report a volume
larger than the day.

**The gate was mutation-tested, and it needed to be.** Five plausible
implementation mistakes, each applied to the replayer in turn:

| mutation | queue feed | bench feed | unit tests |
|---|---|---|---|
| drop the tape print | caught | caught | caught |
| walk the queue naively | **missed** | caught | caught |
| `execute()` for the remainder | caught | caught | caught |
| clamp the print to resting size | **missed** | **missed** | caught |
| check only `original_ref` on a replace | **missed** | **missed** | caught |

None survived, but the full-day gate alone would have missed two. No
well-formed feed — generated or real — executes more shares than the order it
names, so `execute()`'s documented behaviour of recording the *message's* share
count is reachable only from a hand-built message. The unit suite is a gate
component, not a formality. And the queue feed, whose executions are always at
the touch, cannot exercise the skip-historical-orders-ahead rule at all: the
generated feed phase 6 built *because* it respects priority is the one that
cannot test this, and `bench.gz`, whose executions are scattered and which is
wrong as a market, is the one that can.

**The mutation harness had the bug it was looking for.** Its first run scored
the queue feed as catching all five. It caught none: WSL wipes `/tmp` between
invocations, the feed was gone, the gate died on a missing file, and "nonzero
exit" was being read as "mutation caught". A detector that reports success when
it did not run is the failure this repository has already shipped once, in a
receiver that returned before reading its own counters. The rerun keeps its
artifacts somewhere that survives, requires a green baseline before any mutation
is scored, and treats a detector that could not run as an error rather than a
catch.

**What this does not establish.** `strategy_shares_taken` is 0 and
`historical_orders_ahead` is 88,133 across the day, which means the *skip* half
of the rule ran 88,133 times on real data and the *fill* half ran zero times —
it is exercised only by `test_strategy_ahead_fills`, on hand-built messages.
Nothing here emits ITCH, and nothing here has been through a socket. P1 is 12.2.

### 12.2 — The emitting matcher, and P1

Every matcher mutation produces its ITCH message — `A`/`F` on accept, `E` on
execution, `X` on partial cancel, `D` on delete, `U` on replace, `P` for
non-displayed executions if hidden slices trade — sequenced, timestamped,
MoldUDP64-wrapped by the existing `mold` layer, published on UDP. Your exchange
now produces the wire format it consumes, which is the sentence that makes the
project title honest.

**This is where P1 is graded, and P1 decides the phase.** With zero strategy
orders, the book built from the *emitted* feed must be byte-identical to the
phase-9 book built from the *original* feed — the emitted stream graded by your
own phase-3 machinery, so the loop grades itself. It runs on data already in
hand, and it is built **before** OUCH and SoupBinTCP exist rather than after,
because if it fails the topology is wrong and every line of protocol work
written on top of it was wasted.

Determinism arrives here too, because every test after this one leans on it:
fixed seed + fixed inbound script ⇒ byte-identical emitted ITCH, hashed and
checked in CI the way phase 10's gate already is.

**Done when:** P1 passes byte-for-byte on a full day; the determinism hash is in
CI, and a deliberate one-field change is shown to break it.

### 12.2 — what actually happened

The exchange publishes now. `include/itchbook/emit/itch_encode.hpp` writes the
fifteen message types this project models, `include/itchbook/emit/sink.hpp` is
where they go, and the split replayer emits one message per mutation it
performs. `tools/split_replay_gate.cpp` grew a third book.

**P1 passes on the full day.** 268,744,780 messages in,
**264,496,253 ITCH messages published**, and the book a
consumer rebuilds from that published feed is identical to the phase-9 book from
the original feed — **0 divergences after every
message, 0 at the close**, per-symbol CSV
byte-identical. Recorded in `validation/p1-emitted-2019-12-30.json`.

**P1 was nearly vacuous, and the fix was to make it byte-level.** With zero
strategy orders the exchange performs exactly the mutations the feed describes,
so "the book from the published feed equals the book from the original feed"
risks testing very little: it compares two books built by one decoder against a
stream written by that decoder's inverse. The stronger form costs nothing —
carry the header across, re-encode every body field, and require the published
message to come out **byte-identical to the input**. That scores the fields a
book consumer never reads, which is most of them: the timestamp, the tracking
number, match numbers, the stock symbol on `A`/`F`/`P`/`Q`/`H`, the MPID, the
halt reason. **264,496,253 of
264,496,253 published messages are byte-identical to their
input; 0 differ.**

**The first run was not.** It reported 8,906 byte-differences, every one a Stock
Directory message differing from offset 25 — one per symbol. Bytes 25..38 of an
`R` carry issue classification, authenticity, the short-sale threshold, the IPO
flag, the LULD tier and the ETP flags, none of which this project has ever
decoded, and the encoder was zeroing them. There is one right thing to do with
content you are republishing and do not understand, and it is to carry it
across verbatim. Those fourteen bytes are the one place the emitter is a relay
rather than an encoder, and P1's byte-identity therefore says nothing about
them; everything else in every message is re-encoded from a decoded field.

**A bug P1 is structurally incapable of finding, found by review instead.** The
publisher emits one `E` per fill. The replayer recorded one print per input
execution. Those agree at zero strategy orders — one fill per execution — and
disagree the moment a strategy order is in the queue: the subscriber replaying
two `E` messages calls `Book::execute` twice and `record_trade` does `++trades_`
each time, so the exchange's own book would have counted one trade where its
published feed described two. Volume is unaffected, which is why it is easy to
miss. The rule is now **one print per fill**, and `test_strategy_ahead_fills` —
written during 12.1 — had to be corrected, because it had pinned the defect
rather than caught it.

**Queue order is now a gated fact.** Nothing the gate previously compared could
see intra-level order: `resting_orders`, `resting_shares`, `best_bid` and
`best_ask` are all insensitive to the sequence of a level's queue, so a
publisher that emitted two adds in the wrong order, or the two halves of a split
fill back to front, would have passed everything and mis-ranked every queue
position in 12.7. The comparator now hashes reference, shares and price in queue
order for the front 16 of each side, on all three books, after every routed
message.

**An independent decoder reads what we published.** P1's remaining structural
blind spot is that the emitter writes at offsets from `messages.hpp` and the
consumer reads at offsets from `messages.hpp`, so an error in this project's
*model* of the wire cancels out and the round trip closes anyway —
`make_sample.py` states the same limitation about its own builders. The standing
answer in this repository is the reference implementation, so the published
stream is handed to `python/reference/replay.py` and its daily summary must
match the summary it produces from the original feed. That is a second,
separately written decoder rather than a proof about NASDAQ's wire, and the
distinction is the point.

**Determinism.** Fixed input, byte-identical output, checked twice over and
against a stored hash: two runs, because a stored constant cannot tell
deterministic from broken-the-same-way-every-time; and against the constant,
because two identical runs cannot tell correct from drifted-together. The input
is `make_sample.py`, which regenerates from a committed script with no seed to
lose and carries every type the book models. The hash is in
`validation/emitted-itch-sha256.txt`, and CI runs the gate plus a self-test that
corrupts one byte of one published message and requires the gate to refuse.

**Mutation-tested, and it found the same class of hole 12.1 did.** Six publisher
mistakes: `C` always printable, the tracking number reissued as zero, the Stock
Directory tail zeroed, `put32` endianness flipped, a published execution naming
the wrong order, cancels not published. None survived — but "tracking number
reissued as zero" survived every offline detector on the first pass, because
`make_sample.py`'s `header()` hard-codes tracking to zero, so both generated
feeds *and* the hand-built test messages carried zero and the mutation was a
no-op. It is non-zero on 2.9% of a real day, so only the licensed-data run would
ever have caught it. The round-trip test now stamps a non-zero tracking number
on every message it builds.

**What this does not establish.** P1 cannot falsify 12.0's decomposition. At
zero strategy orders the aggressor's queue-walking path is dead code — the
strategy-reference list is provably always empty — so the published stream is a
mechanical re-encode of the input and the round trip closes for reasons that
have nothing to do with whether adds-as-state and executions-as-events is the
right split. That question is settled by 12.1's gate, not this one. The
multi-fill emission path is covered only by `test_split_fill_emits_two`, on
hand-built messages, and stays that way until 12.7 puts real strategy orders in
the book. Nothing here has been through a socket.

### 12.3 — OUCH 4.2

`include/itchbook/ouch/` — 4.2 is the simpler version and the choice is stated
rather than assumed. Core subset, deviations listed in the header the way
`messages.hpp` does for ITCH, **including which lengths are evidence and which
are still just the spec.** That convention now exists in this repository; use it.

- Inbound: Enter Order `O`, Replace `U`, Cancel `X`.
- Outbound: System Event `S`, Accepted `A`, Replaced `U`, Canceled `C`,
  Executed `E`, Rejected `J`.
- Fixed-width, alpha-padded, big-endian where the spec says so — same
  field-offset table style, same trap-hunting mindset. The token/ref distinction
  is this protocol's version of the locate trap.

**Done when:** the field-offset tables exist with evidence separated from spec;
encode → decode → encode is byte-identical over a corpus covering every message
in both directions; the token↔ref distinction is cross-checked rather than
assumed.

### 12.3 — what actually happened

`include/itchbook/ouch/messages.hpp` (decode) and `include/itchbook/ouch/encode.hpp`
(encode), `tests/test_ouch.cpp` in ctest. OUCH 4.2 chosen over 5.0 — simpler,
and it is the version the build plan named before any code existed.

**There is no live NASDAQ OUCH session anywhere in this project's future** —
that needs a market-participant connection, out of scope for a portfolio
project by construction — so the evidence class the ITCH header uses
(CONFIRMED against a real trading day) cannot be claimed here at all. Every
offset in `messages.hpp` is spec-only, for every one of the nine core message
types, with no exception. What differs from a single spec read is how the spec
was read: three independent extractions of the vendor PDF — a blind
reconstruction from pre-flattened text, a from-scratch font-aware extractor,
and `pdftotext -layout` — landed on identical offsets and lengths for all nine
messages, and every message's fields tile its stated total length with zero
gaps and zero overlaps, checked at compile time via `static_assert`.

**The second extraction found a real bug in the first, in the document
itself rather than in the wire format.** The PDF draws five field-name labels
— never an offset, never a length — with an embedded Identity-H composite
font that a naive byte-level text reader renders as garbage or drops
outright. A byte-for-byte re-implementation of the extractor, this time
walking the PDF's font resources and decoding through each font's own
ToUnicode CMap, recovered all five: **Replaced Message offset 9 is
"Replacement Order Token," not "Order Token"** — the one correction that
would have been easy to get wrong by positional analogy to Accepted Message,
since both messages carry a 14-byte token at that offset and only the name
differs. Two more (`Accepted`/`Replaced` offset 49, "Reference Number" not
"Order Reference Number") turned out to be a real inconsistency in NASDAQ's
own document — the analogous field in the unrelated Order Priority Update
Message uses the longer name — not an extraction artifact. No offset or
length changed anywhere; only field names, and only where a reader would
otherwise have guessed.

**One field remains genuinely unconfirmed at the value level.** Enter Order's
Cross Type (offset 47, length 1) has a solid offset and length across all
three passes, but its permitted value characters are only referenced ("see
Data Types") and the table itself never appears in the extracted text of any
pass. It is exposed as an opaque, unenumerated `char` for exactly that reason
— encoded and decoded, never interpreted.

**The token/reference-number distinction is this protocol's version of the
locate trap**, per `docs/phase12-design.md`, and it is load-bearing rather
than decorative: Order Token is a 14-byte client-chosen alphanumeric string,
day-unique per OUCH account, present on every message in both directions;
Order Reference Number is an 8-byte exchange-assigned integer, present only
in Accepted and Replaced among the nine core types, and it is the value that
becomes the ITCH order reference the matcher publishes once a strategy order
exists — the bridge between OUCH's session-local string identity and ITCH's
wire-level integer identity built in 12.1/12.2. `test_ouch.cpp` checks this
adversarially: a token that is itself all digits, paired with a reference
number that is a different integer, decodes to the right value on each side;
a reference number whose bytes would read as ASCII digits if misinterpreted
as a token decodes as the integer it is, never as text.

**Two price sentinels, checked against the spec's own hex.** The maximum
limit price ($199,999.9900 = 1,999,999,900 = `0x7735939C`) and the
market-order-in-a-cross sentinel ($214,748.3647 = 2,147,483,647 =
`0x7FFFFFFF`, `INT32_MAX`) are each stated by the spec in both decimal and
hex — a self-checking pair, verified independently rather than copied, and
pinned by `static_assert` so a transcription slip fails the build rather than
waiting for a test to notice.

**Mutation-tested, because no real-day gate exists to backstop this the way
ITCH's does.** Five plausible mistakes — two field-offset swaps in decode,
an off-by-one in an encode offset, a write that clobbers one field with
another's data, and a deliberately introduced overlap in the compile-time
span table — all five caught, the last one specifically confirmed to fail via
the intended `static_assert` and not some incidental compile error. The
round-trip test and the span-tiling check are not decorative; they were shown
to fail when the header is wrong, which is the only kind of evidence
available in the absence of a live session.

**What this does not establish.** Nothing here has been through a socket —
that is 12.4. Six more message types the spec documents (Modify Order in both
directions, AIQ Cancelled, Broken Trade, Executed with Reference Price,
Cancel Pending, Cancel Reject, Order Priority Update) are out of the core
subset and unimplemented, not forgotten; Modify Order is additionally marked
"greyed out" by the document's own footer note. And every offset here, no
matter how many independent passes agree on it, remains what the header says
it is: spec-only. No real bytes have ever met it, and none will inside this
project.

### 12.4 — SoupBinTCP

Minimal, and small enough to be its own step: login request/accept/reject,
sequenced data packets, client and server heartbeats, end of session. 2-byte
length + 1-byte type framing. Implementing it upgrades the claim from "parses
OUCH structs" to "implements the session layer real firms log into."

**Done when:** accept and reject paths are both tested; a session survives an
idle period on heartbeats alone; a peer that stops heartbeating is declared dead
within the timeout, and that death is *observable* to a caller — 12.5 is what
makes it flatten.

### 12.4 — what actually happened

`include/itchbook/soupbin/messages.hpp` (decode), `encode.hpp` (encode), and
`session.hpp` — a transport-agnostic `ClientSession` / `ServerSession` state
machine, in `tests/test_soupbin.cpp`, 19 tests, ctest.

**Verified three independent ways, and the second pass found the first
pass's PDF extraction was already unusually clean — a genuine surprise
worth recording rather than smoothing over.** Unlike OUCH's PDF, which drew
five field-name labels through an embedded font a naive extractor rendered
as garbage, this document's tables came through intact on the first read: no
dropped labels, no garbled text, every offset present. A second,
independently-written extraction and a third pass with `pdftotext -layout`
both reproduced identical offsets and lengths for all ten packet types. The
one genuine anomaly found — Logout Request's own Packet Length field says
"Binary" where every other one of the nine remaining tables says "Integer"
for the same field — was traced to the source PDF itself via its raw
coordinate data, not to any extraction artifact.

**The state machine, not the wire format, is where the real risk lived, and
adversarial review ran against a design description BEFORE any of
`session.hpp` existed.** It found six blocker/major defects, none
hypothetical:

- **Clock non-monotonicity.** A backward or repeated `now_ns` — a clock
  hiccup, two threads, a test harness replaying ticks out of order — either
  wraps a naive unsigned subtraction to ~584 years (instant false Dead) or,
  guarded the wrong way, suppresses detection forever. Fixed:
  `elapsed = now > last ? now - last : 0`, unconditionally, everywhere a
  `SilenceTracker` is read.
- **A 15-second timeout silently pre-empting a 30-second one.** The
  spec draws two distinct timeout ideas — the server's steady-state
  client-dead threshold (15s, "typically") and its awaiting-login threshold
  (30s, "typically") — and a design that checks both at once during
  `AwaitingLogin` makes the shorter one always win, silently discarding the
  longer allowance. Fixed by construction: exactly one timeout is ever
  active per phase — `login_timeout_ns` during `AwaitingLogin`, the
  steady-state threshold only once `LoggedIn`.
- **`tick()`'s calling-cadence contract, found independently by two of the
  three review lenses.** A peer that has genuinely gone silent, by
  definition, never makes a socket readable again — if `tick()` is only
  called from an I/O-readable handler, dead-peer detection can never fire.
  Documented as a required usage contract in the file's own banner, in the
  same register as a `static_assert`: not a suggestion.
- **Malformed framing folded into the same `Dead` an ordinary silent peer
  reaches.** An operator debugging "why did my session die" cannot tell a
  decode failure from a dropped cable if both produce the identical value.
  Fixed with a distinct `ProtocolViolation` state, entered synchronously
  from `on_bytes()` — never from the time-driven `tick()` — on an
  unrecognized packet type, a fixed-size packet whose declared length
  disagrees with its own type, or a declared length past
  `max_frame_bytes`.
- **A polled, sticky `Dead` state re-triggering a flatten on every poll
  forever.** `risk::KillSwitch` (`include/itchbook/risk/kill_switch.hpp`)
  gets this right by being idempotent about an action that is safe to
  repeat ("stop trading"); flattening resting orders is not obviously safe
  to repeat, and the review named this transferred assumption explicitly.
  Fixed: `on_bytes()` and `tick()` both return `true` exactly on the call
  that changes `state()`, never again while the same terminal state holds.
- **"Never got in" and "was alive and went dark" reaching the identical
  `Dead`.** A caller cannot decide whether a connect-retry or a genuine
  order-flattening trigger is warranted without knowing which. Fixed with a
  distinct `LoginTimedOut`.

**One asymmetry the review's three lenses did not name, found while
resolving the findings above and confirmed from the spec's own text.** A
naive design applies the 1-second heartbeat-send obligation identically to
both roles. Section 2.2.1 states Login Accepted "will always be the first
non-debug packet sent by the server" — a Server Heartbeat is not a Debug
packet, so sending one before login resolves would violate that sentence.
`ClientSession` has no analogous constraint; nothing blocks a heartbeat
before its already-immediate Login Request. So `ServerSession`'s
send-obligation clock starts only once `accept_login()`/`reject_login()`
runs; `ClientSession`'s starts at construction. Verified in both directions:
`test_server_heartbeat_gated_until_logged_in` confirms zero heartbeats
escape during `AwaitingLogin`/`LoginReceived`, however long the wait;
`test_client_heartbeat_unconditional_from_construction` confirms the client
emits one within the first second even before any reply arrives.

**A second gap, found only by writing the implementation rather than by
review: a well-formed packet that makes no sense in the current state was
silently swallowed, not flagged.** Nothing in the review's literal ask
covered a second Login Accepted arriving after the session is already
`LoggedIn` — but conflating that with ordinary background noise is exactly
the debuggability failure `ProtocolViolation` exists to prevent, so both
classes check "is this type expected in my CURRENT state," not merely "is
it ever legal for my role."

**A third gap, found only by writing the test suite: `dispatch()`'s first
draft counted Sequenced Data packets and dropped their contents on the
floor**, which silently defeated the entire reason the packet type exists —
carrying the OUCH message the whole session is built to move.
`test_application_payload_round_trips_both_directions` is the test that
would have caught this eventually and the fix that closes it now: both
classes take an `app_in` sink, and Sequenced/Unsequenced Data payloads
surface there, header stripped, byte-identical.

**A fourth gap, found only by the tests actually running: `on_bytes()` had
no way to know "now."** The first working draft reset the receive-idle
clock using a cached `activity_now_` member set by whichever `tick()` last
ran — stale, or zero if `tick()` had never yet been called.
`test_clock_backward_safety` caught it directly: a session ticked with a
deliberately backward clock (to prove the monotonicity guard) went `Dead`
on the very next call regardless, because `on_bytes()` had anchored the
receive-idle tracker at time zero. Fixed by threading an explicit `now_ns`
through every entry point that touches a `SilenceTracker` —
`on_bytes()`, `send_unsequenced()`, `send_logout()`, `accept_login()`,
`send_sequenced()` — removing the cache entirely rather than trying to keep
it fresh.

**Mutation-tested against the review's own findings, not a generic list: 8
mutations, 7 caught, 1 genuinely inert.** The survivor removed a guard
against resetting the receive-idle clock after a session reaches a terminal
state — and tracing through why it survived rather than forcing a test to
catch it: `tick()` already returns unconditionally the instant
`is_terminal(state_)` is true, before it ever reads that clock again, so the
guard's removal changes no observable behavior. The guard stays, for
clarity and as a defense against a future refactor of `tick()`'s own
ordering; no test was written to manufacture a catch for dead code, which
would have tested the guard's existence rather than any real system
property.

**What this does not establish.** No real socket exists anywhere in this
file — `on_bytes()`/`tick()` are driven entirely by hand-fed bytes and a
caller-supplied clock, which is what makes the exhaustive timing tests
possible without a real second elapsing, and it is exactly the limitation a
transport adapter closes later. `ServerSession`'s accept/reject decision is
deliberately left to the caller — SoupBinTCP's own text describes an
authentication *scheme*, not a *policy* this class enforces — so nothing
here has validated a real username/password against anything; that is
12.5's gateway. And the same evidence ceiling as OUCH applies without
exception: no live SoupBinTCP session exists or will exist for this
project, so every offset remains spec-only, however many independent
extractions agree on it.

### 12.5 — The gateway and the risk layer

`engine/gateway.hpp` — accepts a SoupBin session, validates OUCH (unknown token,
bad price, wrong state → `J` with reason codes), assigns exchange-side refs, and
maintains the token↔ref map. The risk layer runs **before** the matcher:
`risk/kill_switch.hpp` in-line, price collar against the current book, max order
rate, max position. A tripped switch rejects new flow *and* cancels resting
orders.

The heartbeat timeout from 12.4 wires into the same switch: a dead session
flattens.

**Done when:** flatten-on-trip and flatten-on-session-death are both proven by
tests that **count the cancels**, rather than by asserting the switch changed
state. The switch reaching TRIPPED is not the claim; the book going flat is.

### 12.5 — what actually happened

`include/itchbook/engine/gateway.hpp`, plus a borrowing constructor on
`Matcher` and the OUCH reason-code tables in `ouch/messages.hpp`. 13 tests in
`tests/test_gateway.cpp`; 24/24 in ctest.

**The design review found a blocker that reached past this sub-phase, and it
was right.** `Matcher` owned a private `book::Book`; the phase-12.1 replayer
mutates `book::BookSet` books. Different objects. A price collar built on that
would have measured only the strategy's own orders, and — worse —
`docs/phase12-design.md` §4 rules the arrangement out by name: *"Two books —
a 'market' book the matcher consults and a 'mine' book it owns — would put
your order in a queue of one and hand it a fill rate nothing in reality
supports."* The queue-position claim the whole phase rests on was quietly
false. Fixed here with a borrowing `Matcher(book::Book&, tick)` constructor;
purely additive, so all 23 pre-existing tests (including the property fuzzer)
were the regression check and none changed. A strategy order submitted
through the gateway now rests **third in the shared queue behind two
historical orders**, which is the property §4 exists to guarantee and the
first test in the file.

**"J with reason codes" turned out to be implementable after all.** 12.3 had
left the Rejected and Canceled reason lists as bare characters, because the
explanation columns had not been extracted; the review correctly pointed out
that a gateway cannot answer with codes whose meanings are unknown, and that
anything written there would be invented. The font-aware extraction from
12.3 still had both tables in full. So `ouch::reject_reason` and
`ouch::cancel_reason` now carry the spec's own codes, and every rejection
this gateway sends is one NASDAQ defines for that situation — `'X'` invalid
price, `'S'` invalid stock, `'Z'` shares exceed the safety threshold, `'H'`
halted, and for a flatten, `'Z'` *"System cancel. This order was cancelled by
the system."* Nothing invented.

**Six blockers, each fixed and each pinned by a test:**

- **Flatten keyed on `tick()` cannot see two of the four deaths.**
  `ServerSession::tick()` reaches only `Dead` and `LoginTimedOut`; `Ended`
  (the client's own Logout Request) and `ProtocolViolation` are set inside
  `on_bytes()`. A client that logs out politely while holding resting orders
  is the ordinary case, and the obvious design never flattens it. Now
  level-triggered on `is_terminal(state())` with a latch — the latch, not the
  edge, is what makes it exactly once. Tested through all three reachable
  paths.
- **Flatten's own cancels must not feed the kill switch.**
  `max_messages_per_second` is documented as *our outbound* traffic and its
  window buckets by 100 ms, so flattening a large book in one instant would
  trip the rate limit with the risk layer's own remediation, latch the wrong
  `Trip` reason, force an operator reset, and — since the flatten is itself
  triggered by a trip — recurse. Remediation is not the thing that limit
  bounds. Tested: eight cancels in one instant against a limit of four, switch
  still live.
- **Counting cancels cannot prove flatness.** `Matcher::cancel_meta()` zeroes
  `resting`/`hidden`/`in_book` whether or not `Book::remove()` found the
  reference, so every Meta-side assertion after a flatten is self-confirming;
  a buggy flatten can emit N cancels and leave N orders resting. Flatness is
  asserted against the **book**, and independently by submitting a marketable
  order and requiring zero fills — the one check that depends on neither
  layer's bookkeeping.
- **`Request::id`, the published Reference Number, and the token map must be
  one integer.** `Matcher::rest()` writes `Request::id` into
  `book::Order::ref`, and `split.hpp`'s aggressor walk classifies each resting
  order by `is_strategy_ref(q->ref)`. Had the published reference differed
  from the id, the 12.1 partition would have stopped working silently — the
  strategy's own orders counted as historical and never filled.
- **A per-gateway reference counter collides permanently.** `Matcher::orders_`
  is never erased from, so a duplicate id is not transient: `validate()`
  returns `DuplicateId` for it forever. References now come from one
  venue-scoped `RefSource`.
- **Replace has no atomic path in the engine**, and `cancel_meta()` is
  irreversible. Every check on the replacement therefore runs to completion
  *before* the original is touched; a rejected replacement leaves the original
  resting and its token still usable.

Two more the review caught that a first draft would have got wrong: **flatten
filters on `!is_terminal(state)`, not on resting shares** — a stop order
parked awaiting its trigger has `resting == 0`, survives a shares-based
filter, leaves the book flat, passes a count-the-cancels test, and stays armed
— and **the kill switch gates on intent, not message type**: Enter and Replace
both create exposure and both fail closed, while Cancel is never gated,
because a tripped gateway that refuses inbound cancels blocks the only client
action that reduces risk.

**Mutation-tested: 10 mutations, 0 survived — but only after the mutation test
found two of the tests were asserting the wrong thing.** The parked-stop test
cancelled through `Matcher::cancel()` directly, which exercises the matcher
rather than `Gateway::flatten()`'s filter, so reverting the filter left it
green. And the two-gateway test only checked that two references differ —
which two independent per-gateway counters also satisfy, since both allocate
in the same order. Both now assert the claim that matters: the stop is driven
through the gateway's own flatten and then through its trigger price to prove
it cannot come back, and the reference test asserts the shared counter
advanced across both gateways and that a *third*, later gateway does not
reissue an earlier one's reference.

**What this does not establish.** Nothing here has been through a socket —
`on_ouch()` takes a payload `ServerSession` already stripped, and no transport
exists yet. Authentication is the caller's decision, passed in as a boolean:
SoupBinTCP describes a scheme for carrying credentials, not a policy for
judging them, and this gateway does not invent a credential store. The
pre-trade position check duplicates `sim::RiskLimits::blocks()` rather than
sharing it — that type lives inside the backtester header and is typed on a
different `Side` enum — which is a real divergence risk at the boundary and is
recorded here rather than hidden. And `adopt_for_test()` exists on `Gateway`
for exactly one reason, named as such: OUCH 4.2's core subset has no stop
type, so the parked-stop case cannot be reached through the wire path at all.

### 12.6 — Fuzz, and the cross-protocol differential

`tests/fuzz/fuzz_gateway.cpp` — random valid and invalid OUCH byte streams:
malformed lengths, unknown tokens, replace-after-fill races, cancel-of-cancel.

The differential is house style: every OUCH `E` has a matching ITCH `E`/`P`;
token-level shares conserved across both streams; the book built by replaying
the emitted ITCH equals the matcher's internal book.

**Done when:** 1M ops run clean in CI under ASan/UBSan with no crash, **every
inbound message answered exactly once**, and the OUCH↔ITCH invariants hold
across the whole corpus.

### 12.6 — what actually happened

`tests/fuzz/fuzz_gateway.cpp`, plus the five things that had to exist first
before it could test anything. 25/25 in ctest; **1,199,994 operations clean
under ASan/UBSan in 2m02s**.

**The sub-phase opened by finding that the exchange could not trade.** Two
defects, each demonstrated with a running program before a line was written to
fix it, and both created by 12.5's shared book rather than present all along:

    // strategy sends a marketable buy at $100.01 into a $100.00 offer
    before: filled=0 resting=200  crossed=1     <- book left CROSSED
    after : filled=200 resting=0  crossed=0     <- offer down to 300

`Matcher::match()` broke out of its loop whenever the resting order at the
front had no `Meta`, with a comment reading *"not ours; cannot happen
in-engine"*. That was true while the Matcher owned a private book. 12.5 gave
it a book shared with the replayer, and "not ours" became the ordinary case —
most resting orders are historical. A marketable strategy order did not fill
at all; it rested through the opposite side and left the book crossed. This
was raised in the 12.1 review as the "external makers" question and correctly
set aside then, because that design did not route the aggressor through the
Matcher. 12.5 made it apply.

    // the aggressor takes a strategy order resting ahead of the named one
    before: matcher STILL thinks our order rests 100   <- gone from the book
    after : matcher thinks our order rests 0           <- fill recorded

The other direction. `SplitReplayer::aggress()` called `book.take()` directly,
so a strategy order it consumed vanished from the book while `Meta::resting`
stayed stale and the client was never told it had filled. **`conserves_shares()`
returned true across the entire divergence**, because it checks `Meta` against
itself and never against the book — which is why 12.6 also adds
`agrees_with_book()`, the predicate that can see this class of bug at all.

**The arithmetic that makes an external fill different.** `filled_total_`
counts *both* sides of an internal fill, because both sides are `Meta`s and
both get `m.filled += qty`. An external fill has exactly one side in this
engine, so it must add `qty`, not `2 * qty`, or `conserves_shares()`'s
`filled == filled_total_` fails immediately. That in turn breaks
`volume_traded()`, which halves the total — hence a companion
`external_filled_` counter so the halving applies only to the internal part.

**Three more gaps had to close before the differential could mean anything.**
Nothing published ITCH for any matcher mutation, so a subscriber rebuilding
from the feed could not even hold the order the replayer then executed — it
would count that execution as an unknown reference. `ouch::encode::executed`
had **zero call sites in the repository**, so "every OUCH Executed has a
matching ITCH E" was vacuously true at zero Executed messages. And
`engine::Fill` carried no match number, so the join key pairing the two
streams did not exist. All three now do, driven off one cursor over
`Matcher::fills()` — because a fill reaches the gateway two ways, our order
crossing (inside our own `submit`) and our resting order being hit (during the
replayer's `apply`, with no call of ours on the stack), and only a cursor sees
both.

**Two tests were passing for the wrong reason, and the fixes exposed them.**
The collar test's first order sat *above* the historical ask — marketable —
and only rested because the engine could not trade; once it could, it consumed
the ask and left nothing for the collar to measure. Two more assertions asked
"what was the last wire message" when they meant "was the order accepted",
which were the same question only while orders never filled. Both rewritten,
the collar test with an additional just-inside-the-collar case so it cannot
pass by rejecting everything.

**The differential caught a modelling error in its own harness within 103
iterations.** The first version routed only the *gateway's* ITCH to the
subscriber. A real subscriber sees one feed — the union of what the replayer
publishes for historical mutations and what the gateway publishes for the
engine's — so the aggressor's executions against strategy orders never
arrived and the subscriber held those orders at full size. With both wired the
check also got stronger: the subscriber's book is now compared to the
exchange's **in full and in both directions**, so a feed that invents orders
fails as surely as one that omits them.

**Coverage is asserted, not assumed.** The binary exits non-zero if a run
never reached the two fill paths the phase exists to differentiate, because
both were completely broken before this sub-phase and a run that misses them
is not evidence of anything. Over the million-operation CI run:

| | |
|---|---|
| strategy crossed historical liquidity | 77,542 |
| aggressor hit a resting strategy order | 1,689 |
| OUCH Executed / ITCH published | 271,678 / 805,026 |
| malformed / duplicate / unknown-token | 84,470 / 31,481 / 83,854 |

**Built with sanitizers, against the local precedent.** `fuzz_matcher` opts
out with `-fno-sanitize=all` and a comment estimating ASan at ten times the
wall clock. That was a throughput decision for a 200,000-sequence run, not a
claim that sanitizers are unaffordable — measured here at ~110k ops/s under
ASan/UBSan, so the phase's million operations take two minutes. The
done-condition says ASan/UBSan explicitly and a sanitizer-free binary would
not satisfy it.

**The gate was watched failing.** Suppressing the ITCH delete the gateway
publishes on a cancel makes the subscriber's book drift, and the differential
refuses on iteration 1 with "subscriber holds a different amount than the
exchange". That negative test runs in CI beside the positive one.

**What this does not establish.** Still no socket: `on_ouch()` takes a payload
`ServerSession` already stripped, and the two sides are wired in-process. The
kill-switch and flatten paths are reachable in the fuzz but the generator does
not currently drive the switch to trip, so those two counters read zero — the
unit suite covers them and the fuzz does not, which the coverage report shows
rather than hides. The price collar is disabled in the fuzz config, because
with it on most generated traffic is rejected and the interesting paths go
unexercised; it is unit-tested instead. And `pump_stops()` is called after each
historical execution, which is the right place, but no generated sequence yet
parks a stop that a later external fill elects.

### 12.7 — The closed loop

Three processes: the historical-state replayer of 12.1, the exchange (gateway +
matcher + ITCH publisher), and the strategy (phase-10 receiver + ring + book on
the ITCH side, SoupBin client on the OUCH side, `InventoryStrategy` in the
middle).

Two clocks, per 12.0. The market runs in **replay time** — it drives message
timestamps and the strategy's `T − t`. Tick-to-trade is **wall clock**. Feeding
wall clock to an A–S horizon during a 50× replay changes the strategy rather
than the load, which is a silent experimental error and therefore wants an
assert rather than a paragraph.

**Done when:** the strategy trades against your own exchange over real sockets,
inside a replayed historical day, under the 12.0 topology — with its own fills
observed back through its own feed handler, not reported to it by the exchange
side.

### 12.8 — Tick-to-trade, decomposed

One histogram per hop, so the total decomposes — and with log buckets, a *shape*
per hop, not just a percentile:

- **t₀** packet arrival at the strategy socket → **t₁** book updated (phase 10's
  number)
- **t₁ → t₂** strategy decision
- **t₂ → t₃** OUCH Enter written to socket — **t₀→t₃ is tick-to-trade**, the
  headline
- **t₃ → t₄** gateway accept; **t₄ → t₅** match + ITCH publish; **t₅ → t₆** own
  fill observed back at the strategy — the full round trip.

Stacked bar at p50 and p99.9.

**This step needs bare metal.** Phase 10 established that the WSL2 box cannot
hold a core and that its noise floor sits above the numbers being measured; a
seven-hop decomposition is strictly more sensitive to that than the one-hop
number was, because the per-hop figures are smaller than the total. Budget the
boot, rather than budgeting around it.

**Done when:** p50 and p99.9 are reported per hop and the hops sum to the total;
**any hop you cannot explain means you have not finished.**

### 12.9 — Replay-vs-live A/B, and the results document

Identical historical day, identical strategy, twice: (a) through the phase-6/11
backtester, (b) live through the loop, with (a)'s latency model set to the
*measured* hop latencies from (b). Diff the fills. The differences are precisely
the backtester's modelling error — queue approximation, latency-model shape,
tie-breaks — and now they are enumerable. `docs/phase12-results.md` is built
around that table, generated from artifacts like every other results table in
phases 9–12.

Caveats stated up front, both of them: your orders consume liquidity the
historical participants never saw (no market impact model in either lane), and
under the 12.0 hybrid those participants never react to you at all. Perfect
agreement is not expected; **explained** disagreement is the bar.

**Done when:** the A/B is published and every disagreement is categorised. A
disagreement categorised as unexplained is a finding and may stand; an
*uncategorised* one is an unfinished phase.

### Done — Phase 12

- [x] **12.0** — topology decided and written before the gateway exists.
      [`docs/phase12-design.md`](phase12-design.md).
- [x] **12.1** — replayer splits adds from executions; reference partition
      armed; zero-order replay equals the phase-9 book.
      *(268,744,780 messages, 0 divergences after every one, 0 partition
      violations, per-symbol CSV byte-identical —
      `validation/split-replay-2019-12-30.json`. Two of the design doc's
      classifications corrected by measurement: `C` and `P` are state, not
      aggressors.)*
- [x] **12.2** — **P1:** book from the emitted feed byte-identical to the
      phase-9 book from the original feed; determinism hash green in CI.
      *(264,496,253 messages published from a
      268,744,780-message day, 0 book divergences
      after every one, 264,496,253 byte-identical to their
      input — `validation/p1-emitted-2019-12-30.json`. The emitted-ITCH hash is in
      `validation/emitted-itch-sha256.txt` with a CI self-test that must catch
      one corrupted message.)*
- [x] **12.3** — OUCH 4.2 core subset; deviations listed with evidence separated
      from spec; round-trip byte-identical.
      *(Nine core message types, offsets triangulated three independent ways
      from the vendor PDF — a font-decoding bug in one extraction pass found
      and fixed by the second. Zero gaps/overlaps per message, checked by
      `static_assert`. Price sentinels verified against the spec's own hex.
      5/5 mutations caught — no real-day gate exists for OUCH, so this is the
      whole defense. Every offset remains spec-only; no live OUCH session
      exists or will exist for this project.)*
- [x] **12.4** — SoupBinTCP session: heartbeats hold an idle session, a dead
      peer is detected within the timeout.
      *(Field tables triangulated three independent ways, one genuine
      spec-drafting slip found. Adversarial design review before any code
      existed found 6 blocker/major defects — clock non-monotonicity, a
      15s/30s timeout conflict, an undocumented tick()-cadence requirement,
      malformed framing folded into Dead, a re-triggering flatten signal, and
      LoginTimedOut vs Dead conflated — all fixed. Two more gaps found only
      by implementing and testing: an out-of-state packet silently swallowed,
      and Sequenced Data payloads dropped on the floor entirely. 8 mutations
      against the review's own findings, 7 caught, 1 traced to genuinely
      inert code rather than forced. 19 tests, ASan/UBSan, gcc + clang.)*
- [x] **12.5** — kill-switch flatten-on-trip and flatten-on-session-death proven
      by tests that assert the BOOK, not the cancel count.
      *(Design review before any code found a blocker reaching past this
      sub-phase: Matcher owned a private book, which `phase12-design.md` §4
      forbids by name. Fixed with a borrowing constructor — additive, all 23
      prior tests unchanged — and a strategy order now rests third in the
      shared queue behind two historical orders. Six blockers fixed in all.
      The OUCH reason-code tables were recovered from the 12.3 extraction, so
      no reject code is invented. 10 mutations, 0 survived — after the
      mutation test found two tests asserting the wrong thing.)*
- [x] **12.6** — cross-protocol differential and gateway fuzz (≥1M ops) clean in
      CI.
      *(1,199,994 ops under ASan/UBSan in 2m02s. Opened by proving the exchange
      could not trade at all: `match()` refused makers it did not own, leaving
      a marketable order unfilled and the book crossed, and the replayer's
      aggressor reduced strategy orders behind the engine's back while
      `conserves_shares()` reported true. Five blockers fixed — external-maker
      matching, `apply_external_fill`, ITCH emission for engine mutations, OUCH
      Executed, and match numbers — before the differential could mean
      anything. Coverage is asserted: the binary fails if a run never reaches
      either fill path. Watched failing on a suppressed ITCH delete.)*
- [ ] **12.7** — strategy trades against your own exchange over real sockets,
      inside a replayed historical day, under the 12.0 topology.
- [ ] **12.8** — tick-to-trade p50/p99.9 reported and decomposed per hop; every
      hop explained. **Needs bare metal.**
- [ ] **12.9** — replay-vs-live A/B published with every disagreement
      categorised.

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
