# Phase 9 — a full NASDAQ day, every symbol, one process

**30 December 2019. 268,744,780 messages. 8,906 securities.**

Phases 1–8 reconstructed one symbol. This one reconstructs the market, and the
interesting result is not that it worked — it is what stopped being true when it
did. Three claims this project had been carrying were refuted by the run, and one
prediction, written down before it, was kept.

Everything numeric below is generated from the artifacts in `validation/` by
`scripts/phase9-report.py`, which CI runs with `--check`. Nothing here was read
off a terminal and typed in.

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
| **accounted** | **509.8** | 92.5% of peak RSS |
| residual | 41.3 | books, directory, overflow maps, allocator, binary |

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

<!-- generated:end -->

## What the numbers mean

### Decompression was never the bottleneck

The plan asserted, for two revisions, that an end-to-end run would be
"decompression-bound by 3–7×". It is not bound by decompression at all: framing
the whole 8.25 GB file costs a fifth of the run. That claim came from an estimate
of zlib's throughput made without measuring zlib, and one flagless pass of
`itch_census` settled it.

The consequence runs forward. Phase 10's reader thread was justified partly by
closing this gap; it can recover a fifth of the wall clock at best, and that is
now what it will be sold as.

### The cache-hot benchmark does not survive scale, and that is the phase

`bench/` reports 22.8 ns per message on a one-symbol feed whose working set is a
few megabytes. The same code over a whole day — 8,906 books, a reference map and
bands that together are most of half a gigabyte — costs several times that per
message. Nothing about the algorithm changed. The working set did.

That was predicted before the run rather than explained after it, which is the
only reason it is worth anything. The census had already priced the mechanism:
adding live-order tracking to a framing pass cost 49 s for ~285 M hash operations
against a 67 MB table — about 172 ns each, memory-bound — and the book's reference
map does the same shape of work.

**And then the prediction stopped being true, because the system got faster.**
The table above grades it twice on purpose. Against the configuration it was
written for — the reference map pre-sized to twice the peak, which was the
default at the time — every one of its three bounds held. Phase 9.9 then swept
the load factor, found 2.42x sitting in it, and moved the default; the same code
on the same file now runs *below* the range predicted for it.

Both rows belong in this document. Deleting the first would hide that the
prediction was right about the mechanism and the magnitude; deleting the second
would leave a claim standing that the current build does not support. An earlier
version of the script that generates this section printed the word "kept" as a
literal beside whatever the latest numbers happened to be, and went on printing
it after they moved outside the range. It computes the verdict now.

### The band is where the design actually failed

A dense array of price levels near the touch is the phase-3 story: the levels that
matter stay in L1. Across 8,906 symbols, at 512 slots per side, the median symbol
has a fifth of its adds outside that array and nearly a third of symbols have more
than half.

The aggregate figure — 13% — is the one a less careful write-up would report, and
it is dominated by a handful of very active symbols whose bands happen to work. The
per-symbol distribution is the honest picture.

Three distinct failures, and they want three different fixes:

1. **Never evaluated.** The re-centre policy looks once, at 1,000 adds. Most
   symbols never get there, so their bands are never checked, and they are the
   worst offenders by mean. They are also cheap: illiquid symbols contribute
   little in absolute terms. A policy that looked at *elapsed session time* rather
   than an add count would reach them.
2. **Judged fine, then drifted.** A handful of large names — FB, BABA, NFLX —
   passed the check at 1,000 adds and ended the day almost entirely off-band. One
   look per session is not enough for a symbol that trades all day. This is the
   expensive failure, and the fix is a standing check rather than a one-shot.
3. **No affordable band exists.** GOOGL, GOOG, AMZN and TSLA are 98–100% off-band
   and re-centring did not help, because at $1,340 a 512-slot penny grid spans
   ±0.19% of the price. The prediction written into the plan before the run said
   the failures would be the high-priced names and that the honest conclusion
   would be that a production system needs a per-symbol tick regime rather than
   one global grid. Both halves held.

None of this is a correctness problem, and that is worth stating plainly: an
off-band level lives in the cold `std::map` and is found by price, so the
reconstruction is identical either way. CI asserts exactly that, sweeping the band
width and requiring byte-identical output. The band is a locality knob, and this
section is a measurement of how well it was set — not of whether the book is right.

### The stub quotes, which no generated feed has

77.6% of the symbols that quoted posted an order at or above $100,000, and 80.2%
posted one at or below $0.01, clustered on $199,999.99, $199,999.00 and
$100,000.00. Those are two-sided quoting obligations parked where they cannot
fill. Nothing on the wire marks one.

They are a permanent, irreducible population in the overflow map, and they are why
a symbol's *quoted* price range cannot size a band: for three symbols in four it
spans the whole price axis. No generator in this repository emits one, so nothing
here would ever have shown it — which is the argument
[`what-synthetic-data-hides.md`](writing/what-synthetic-data-hides.md) was already
making, now with a second worked example from real bytes.

## What was verified

The oracle cannot chew 268 million messages, so verification changed shape:

* **Global invariants**, in `scripts/full-day-check.py`: the census counts from
  the wire with no book at all, the run counts while building 8,906 of them, and
  they must agree. Orders added, orders resting at the close, volume summed across
  symbols, message accounting — ten of them, all holding. Two more could not run,
  because the committed census predates the type histogram they need, and the
  script says so and exits non-zero rather than reporting ten passes as twelve.
* **Zero unknown references** across the whole feed. Not one symbol on one day —
  every reference in 268 million messages named an order the book was holding.
* **Zero locate mismatches.** Every reference resolved to an order belonging to
  the symbol the message named, which is the check `Order::locate` exists for.
* **`--symbol` output byte-identical** to the pre-phase-9 binary, gated in CI, so
  everything above was bought without moving the single-symbol path.

## What is still open

* The band width was set to 512 and graded, not swept. The curve of off-band
  fraction against N is one run per point and has not been produced.
* A per-symbol width, or a per-symbol tick regime, is the obvious answer to
  failure mode 3 and has not been measured. An earlier attempt to derive one from
  the census's price ranges was refuted before it was built, which is not the same
  as having tried it.
* `h`, `W` and `B` did not occur on this day, so their handling has been exercised
  only by a generated feed and their offsets remain unconfirmed against real bytes.
* One trading day. A second is the next thing that would change what is known.
