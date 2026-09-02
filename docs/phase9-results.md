# Phase 9 — a full NASDAQ day, every symbol, one process

**30 December 2019. 268,744,780 messages. 8,906 securities.**

Phases 1–8 reconstructed one symbol. This one reconstructs the market, and the
interesting result is what stopped being true when it did, rather than the fact
that it worked at all. The run refuted three claims this project had been
carrying, and it kept one prediction that had been written down beforehand.

Everything numeric below is generated from the artifacts in `validation/` by
`scripts/phase9-report.py`, which CI runs with `--check`. Nothing here was read
from a terminal and typed in.

<!-- generated:begin -->

## The run

| | |
|---|---:|
| messages | 268,744,780 |
| symbols | 8,906 |
| executed volume | 971,016,019 shares |
| **wall clock** | **44.57 s** |
| throughput | 6.03 M msg/s |
| peak RSS | 551.1 MB |
| unknown references | 0 |
| locate mismatches | 0 |
| undirectoried messages | 0 |
| operational halts ('h') | 0 |
| broken trades ('B') | 0 |
| MWCB ('W') | 0 — no market-wide breach on this day |

## The two numbers, and which is which

| | seconds | M msg/s |
|---|---:|---:|
| decompress + frame + length-check, building nothing | 16.51 | 16.28 |
| the same, plus building 8,906 books | 44.57 | 6.03 |
| **the book's own cost** | **28.06** | — |

Decompression is **37%** of the run.

## The prediction

> book-only 60–120 s, end-to-end 80–140 s, 5–20x worse per message than the cache-hot benchmark

| configuration | book-only | end-to-end | vs cache-hot | inside the prediction |
|---|---:|---:|---:|---|
| as predicted against — 4.19M map slots, 46% load | 68.8 s | 85.3 s | 11.2x | **yes** |
| as it stands — 8.39M map slots, 23% load | 28.1 s | 44.6 s | 4.6x | no |

The single-symbol benchmark reports 22.8 ns per message. Across a whole day of every symbol it is **104 ns**.

## Memory, decomposed

| | MB | what it is |
|---|---:|---|
| dense bands | 291.8 | 512 slots x 2 sides x 8,906 books x 32 B |
| reference map | 134.2 | 8,388,608 slots x 16 B |
| order pool | 83.7 | 2,093,056 orders x 40 B |
| overflow maps (bound) | 41.2 | 571,766 peak levels x 72 B per red-black node |
| **accounted** | **550.9** | 100.0% of peak RSS |
| residual | 0.1 | books, directory, allocator, binary |

**The overflow row is an upper bound, so the 0.1 MB residual is not evidence that the decomposition closed.** The bound sums each side's peak although the two need not peak together, and compares them against a peak RSS that need not coincide with either. Books, the directory and the binary are certainly not 0.1 MB between them, so the real overflow figure sits somewhat below the bound and those terms cover the rest. Measuring the coincident sum would mean one side's insert reading the other side's map, which is why it is a bound and says so.

Peak live orders were 1,924,078; the pool ended at 2,093,056, and the reference map was pre-sized to 8,388,608 slots — 4.36x the peak, which is the load factor that sizing was chosen for.

## The band, graded

Overall, **13.36%** of 140,270,523 adds landed outside the dense band. Per symbol it is far worse, and the aggregate hides it:

| percentile of symbols | adds landing off-band |
|---|---:|
| p10 | 0.9% |
| p25 | 3.8% |
| p50 | 20.5% |
| p75 | 69.1% |
| p90 | 100.0% |

**2,709 of 8,892 symbols (30%) had at least half their adds off-band.**

### Three failure modes, each with a count

| | symbols | mean off-band |
|---|---:|---:|
| evaluated at 1,000 adds and judged fine | 1,343 | 2.5% |
| re-centred once | 4,319 | 23.9% |
| never reached 1,000 adds, so never evaluated | 3,230 | 66.6% |

And 7 symbols were judged fine and drifted out anyway, because the policy looks once and never again:

| symbol | adds | off-band | re-centres |
|---|---:|---:|---:|
| URTY | 790,501 | 55.3% | 0 |
| FB | 288,078 | 89.9% | 0 |
| BABA | 274,670 | 95.9% | 0 |
| NFLX | 172,943 | 87.0% | 0 |
| ASML | 131,618 | 52.6% | 0 |
| HD | 120,659 | 54.9% | 0 |

### The symbols the band failed hardest

| symbol | adds | off-band | re-centres |
|---|---:|---:|---:|
| URTY | 790,501 | 55.3% | 0 |
| GOOGL | 439,123 | 98.5% | 1 |
| GOOG | 365,128 | 99.9% | 1 |
| AAPL | 795,712 | 40.7% | 1 |
| AMZN | 287,475 | 99.0% | 1 |
| TSLA | 286,674 | 97.9% | 1 |
| BABA | 274,670 | 95.9% | 0 |
| TQQQ | 874,084 | 29.8% | 0 |

## Overflow, distributed

The dense band is the fast store and the `std::map` behind it is the slow one, so *how much* fell through matters as much as the off-band percentage above. It is reported here rather than folded into the residual row, which is what made that row an aggregate hiding a distribution.

**At the close, 0 overflow levels stood across all 8,906 symbols, in 0 of them.** That number is structural rather than small: the session ends flat, and the book erases each overflow level as it empties to keep the map cold. A completed day reports an empty map however hard overflow was worked in between — and it was worked, 18,739,843 times. The terminal size cannot decompose peak RSS, which is itself a high-water mark.

**8,892 of 8,906 symbols (99.8%) used overflow at some point**, holding 571,766 levels between them at their respective worsts.

| percentile of symbols using overflow | peak levels held |
|---|---:|
| p10 | 11 |
| p25 | 19 |
| p50 | 36 |
| p75 | 69 |
| p90 | 119 |
| p99 | 475 |
| max | 5,593 |

### The symbols that leaned on overflow hardest

| symbol | peak overflow levels | adds | off-band | re-centres |
|---|---:|---:|---:|---:|
| AMZN | 5,593 | 287,475 | 99.0% | 1 |
| AAPL | 4,997 | 795,712 | 40.7% | 1 |
| MSFT | 3,455 | 630,899 | 8.3% | 0 |
| TSLA | 3,455 | 286,674 | 97.9% | 1 |
| NVDA | 2,984 | 244,389 | 90.1% | 1 |
| FB | 2,626 | 288,078 | 89.9% | 0 |

## The wall clock did not reproduce, and the book is not why

On 2026-08-24 every figure above was re-measured. Each **deterministic** one came back identical — 231,556 per-symbol cells compared, 0 differing. The wall clock did not: **52.49 s against the 44.57 s recorded**, 1.18x. So the arms were run alternately inside one session, sharing whatever load there was, because running one to completion and then the other is how a busy machine gets attributed to a code change.

| binary | runs | best |
|---|---:|---:|
| dfe2837 - the commit whose run recorded 44.57 s | 2 | 52.49 s |
| HEAD - 49 commits later, unmodified | 2 | 53.92 s |
| HEAD + peak-overflow and MWCB fields | 4 | 53.20 s |

The commit that recorded 44.57 s reports 52.49 s today — its own binary, the same file, byte-identical output. All three arms sit within 1.43 s of each other, so neither the 49 commits of phase 10 and 11 work nor the counters added for this section cost anything measurable. What moved is the machine.

Ruled out, each by measurement rather than by argument:

* **power state** — battery min 52.43 s, AC min 53.20 s - AC did not help, so a power cap does not explain it
* **build flags** — -march=native measured 52.97 s, indistinguishable
* **io** — the framing-only pass moved 16.51 -> 17.41 s (+5%) while the book pass moved +21%; an I/O or decompression cause would move both

What is left is contention: the machine was not idle; top consumers were WindowServer and application helpers, which compete for the single performance core a synchronous book_replay needs, on a machine with 6 performance cores and a 1-minute load average between 4.04 and 6.94 across the runs.

**The recorded numbers above are kept rather than replaced.** They were taken on a quieter machine, and a performance figure should measure the program rather than what else was running — the same reason the sweeps in this phase report the minimum of their samples. `timing_provenance` in each artifact names the two fields this applies to; every other field in them is from the re-run.


<!-- generated:end -->

## What the numbers mean

### Decompression was never the bottleneck

The plan asserted, over two revisions, that an end-to-end run would be
"decompression-bound by 3–7×". The run is not bound by decompression at all:
framing the whole 8.25 GB file costs a fifth of it. That claim came from an
estimate of zlib's throughput made without measuring zlib, and a single flagless
pass of `itch_census` settled the matter.

The consequence carries forward. Phase 10's reader thread was justified partly
by closing this gap. It can recover a fifth of the wall clock at best, and that
is now the claim made for it.

### The cache-hot benchmark does not survive scale, and that is the phase

`bench/` reports 22.8 ns per message on a one-symbol feed whose working set is a
few megabytes. Over a whole day the same code costs several times that per
message, with 8,906 books and a reference map and bands that together come to
most of half a gigabyte. Nothing about the algorithm changed. The working set
did.

That was predicted before the run rather than explained after it, which is the
only reason it is worth anything. The census had already priced the mechanism.
Live-order tracking added to a framing pass cost 49 s for ~285 M hash operations
against a 67 MB table, about 172 ns each and memory-bound, and the book's
reference map does the same kind of work.

**And then the prediction stopped being true, because the system became faster.**
The table above grades it twice deliberately. Against the configuration it was
written for, with the reference map pre-sized to twice the peak, which was the
default at the time, every one of its three bounds held. Phase 9.9 then swept
the load factor, found 2.42x available in it, and moved the default. The same
code on the same file now runs *below* the range predicted for it.

Both rows belong in this document. Deleting the first would hide that the
prediction was right about the mechanism and the magnitude, and deleting the
second would leave a claim standing that the current build does not support. An
earlier version of the script that generates this section printed the word
"kept" as a literal beside whatever the latest numbers happened to be, and it
continued to print it after they moved outside the range. The verdict is
computed now.

### The band is where the design actually failed

A dense array of price levels near the touch is the phase-3 story: the levels
that matter stay in L1. Across 8,906 symbols, at 512 slots per side, the median
symbol has a fifth of its adds outside that array, and nearly a third of symbols
have more than half.

The aggregate figure of 13% is the one a less careful write-up would report, and
a handful of very active symbols whose bands happen to work dominate it. The
per-symbol distribution is the honest picture.

Three distinct failures, and they want three different fixes:

1. **Never evaluated.** The re-centre policy looks once, at 1,000 adds. Most
   symbols never reach that count, so their bands are never checked, and they
   are the worst offenders by mean. They are also cheap, since illiquid symbols
   contribute little in absolute terms. A policy keyed to *elapsed session time*
   instead of an add count would reach them.
2. **Judged fine, then drifted.** A handful of large names, among them FB, BABA
   and NFLX, passed the check at 1,000 adds and ended the day almost entirely
   off-band. One look per session is not enough for a symbol that trades all
   day. This is the expensive failure, and the fix is a standing check in place
   of a one-shot.
3. **No affordable band exists.** GOOGL, GOOG, AMZN and TSLA are 98–100% off-band
   and re-centring did not help, because at $1,340 a 512-slot penny grid spans
   ±0.19% of the price. The prediction written into the plan before the run said
   the failures would be the high-priced names, and that the honest conclusion
   would be that a production system needs a per-symbol tick regime instead of
   one global grid. Both halves held.

None of this is a correctness problem, and that deserves to be stated plainly:
an off-band level lives in the cold `std::map` and is found by price, so the
reconstruction is identical either way. CI asserts exactly that, with a sweep of
the band width that requires byte-identical output. The band is a locality
parameter, and this section measures how well it was set rather than whether the
book is right.

### The stub quotes, which no generated feed has

77.6% of the symbols that quoted posted an order at or above $100,000, and 80.2%
posted one at or below $0.01, clustered on $199,999.99, $199,999.00 and
$100,000.00. Those are two-sided quoting obligations placed where they cannot
fill. Nothing on the wire marks one.

They form a permanent and irreducible population in the overflow map, and they
are why a symbol's *quoted* price range cannot size a band: for three symbols in
four it spans the whole price axis. No generator in this repository emits one, so
nothing here would ever have shown it. That is the argument
[`what-synthetic-data-hides.md`](writing/what-synthetic-data-hides.md) was already
making, now with a second worked example from real bytes.

## What was verified

The oracle cannot process 268 million messages, so verification took a different
form:

* **Global invariants**, in `scripts/full-day-check.py`: the census counts from
  the wire with no book at all, the run counts while building 8,906 of them, and
  the two must agree. Orders added, orders resting at the close, volume summed
  across symbols and message accounting come to ten such invariants, all of them
  holding. Two more could not run, because the committed census predates the type
  histogram they need, and the script says so and exits non-zero instead of
  reporting ten passes as twelve.
* **Zero unknown references** across the whole feed. This covers more than one
  symbol on one day: every reference in 268 million messages named an order the
  book was holding.
* **Zero locate mismatches.** Every reference resolved to an order belonging to
  the symbol the message named, which is the check `Order::locate` exists for.
* **`--symbol` output byte-identical** to the pre-phase-9 binary, gated in CI, so
  everything above was obtained without moving the single-symbol path.

## What is still open

* The band width was set to 512 and graded, and it was not swept. The curve of
  off-band fraction against N is one run per point and has not been produced.
* A per-symbol width, or a per-symbol tick regime, is the obvious answer to
  failure mode 3 and has not been measured. An earlier attempt to derive one from
  the census's price ranges was refuted before it was built, which is not the same
  as having tried it.
* `h`, `W` and `B` did not occur on this day, so their handling has been exercised
  only by a generated feed, and their offsets remain unconfirmed against real
  bytes.
* One trading day. A second is the next thing that would change what is known.
