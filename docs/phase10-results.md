# Phase 10 — the rate–latency curve

Every latency figure in this repository before phase 10 was **handler cost**:
cycles between two `rdtsc` reads around a function call, one thread, over a feed
already in memory. 82 cycles per message is a true statement about a function
and it is not a latency. This phase measures the thing a trading system means by
the word — bytes arriving to book updated, across a thread boundary, through a
queue, under a load that may exceed what the consumer can absorb.

> **Read `docs/phase10-methodology.md` first.** It was written before any of
> this existed and it is the reason several decisions below look more paranoid
> than the code needs.

## Read the caveats before the numbers

The tables below come from a **pinned run on a real machine** — eight physical
cores, three threads on three distinct ones, an invariant TSC, and a cross-core
offset bounded under 47 ns. That is a different situation from the one this
section used to describe, and exactly one of the three obstacles has been
removed.

**The clock is settled.** `tsc_offset` pins two threads to two separate physical
cores and ping-pongs 200,000 samples in each direction. The offset estimate comes
back smaller than the method can resolve — bounded under 47 ns, not distinguishable
from zero — against a wire-to-book latency in the microseconds. On the earlier
container it reported UNMEASURABLE; on a Mac it cannot report at all, because
Apple silicon has no thread-affinity API. This is the phase 10 done-item asking
for the cross-core offset measured and reported, and it is met.

**The sweep is still not a result, and the reason is unchanged.** `mold_replay_udp`
measures its own lateness and reports it per rate. Its p99.9 lateness exceeded the
10 µs bar at **every rate on the ladder**, including 1× real time, so the numbers
below describe the load generator rather than the pipeline. The machine runs
Linux under a hypervisor: threads pin to vCPUs, and the vCPUs are scheduled by
something this process cannot see. Pinning inside a VM is real and insufficient.

So one caveat has gone and one has hardened:

1. **The sender still cannot hold its schedule** — now demonstrably a property
   of the nested scheduler rather than of core contention, since three threads
   had three physical cores to themselves and it made no difference to lateness.
2. **Pinning is real here**, unlike before. `pinned` reads true, and the cores
   chosen are on distinct physical cores rather than SMT siblings — the default
   choice of the last three CPUs would have put two threads on one core while
   reporting all three as pinned.
3. **The arrival and completion stamps come from the same clock family**, and
   the offset between the two cores reading it is bounded at 47 ns. Their
   difference means something now.

What remains is a load generator that can hold a sub-10 µs schedule. That needs
bare metal with isolated cores, not a VM — `isolcpus` is not available under
WSL2 and the governor is invisible to the script. `scripts/pinned-run.sh` is the
run; it needs a different host.

**Pinning alone is not enough, and this has now been measured twice.** In the
container, best-of-five sender lateness went from 562 µs unpinned to 39 µs
pinned — a 14× improvement that still missed the 10 µs bar by 4×. On this host,
with genuinely distinct physical cores, the bar is missed at every rate anyway.
Both runs point at the same thing: the scheduler underneath, not the placement
above it.

## What the sweep does

`bench/rate-sweep.py` offers the same feed at a ladder of rates and records what
comes out. Four decisions in it are worth stating, because each was a way of
being wrong:

**One times real time is computed, not quoted.** The ladder is anchored to the
feed's own clock — `itch_census --timing` reports the span between the first and
last timestamp in the file, and the base rate is the message count over that
span. A real-time figure typed into a script is a constant that drifts from the
feed it claims to describe. (Standing rule 7 applies to inputs, not only to
outputs.)

**Best of N, not the median.** Latency noise here is one-sided: a descheduled
thread or a competing process can only make a run slower. The best run at each
rate is the one that measured the pipeline rather than the machine's other
tenants — the same reasoning phase 9.9 used after a single outlier put the
within-variant spread of a reproducible 1.9× effect at 190%. Runs that dropped
and runs that did not are never pooled: a run that lost half the feed did almost
no work, and letting it win its rate is how a cliff gets smoothed into a slope.

**The ladder extends itself until something drops.** If the top rung is still
clean, the sweep doubles and runs again. Otherwise the "max sustainable rate" is
a fact about how high the script was told to count, and it is reported as a
lower bound (`≥`) rather than as a measurement whenever the cliff was not
actually reached.

**Kernel drops and ring drops are never added together.** On loopback the
socket buffer overflows before the ring does. A sustainable rate requires zero
of both, and `/proc/net/udp` being unreadable is reported as UNKNOWN rather than
as zero — "no drops" and "this platform cannot tell you" are different claims.

<!-- generated:begin -->

## What was run

| | |
|---|---:|
| feed | 5,032,462 messages over 60.0 s of session |
| one times real time | 83,849 msg/s |
| ring | 65,536 slots |
| clock | rdtsc / rdtscp (per-core counter) |
| cross-core clock offset | **bounded under 47 ns**, not measured — the estimate is smaller than the method can resolve (rdtsc / rdtscp (per-core counter)) |
| threads pinned | yes |
| runs per rate | 5 (best of) |
| rates on the ladder | 9 |
| rates where the sender held its schedule | 0 of 9 |

## The curve

Kernel drops and ring drops are separate columns, never a sum. On loopback the socket buffer overflows before the ring does, so a table that added them could not tell a pipeline that is drowning from one whose water came in upstream.

Offered is what the sender was told to send; achieved is what it managed. They are the same number only while it keeps its schedule, and the pipeline can only have absorbed the second one.

| offered | achieved | × real time | p50 ns | p99 ns | p99.9 ns | ring-full | kernel | mid-block | peak occupancy | sender p99.9 late | verdict |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 83,849 | 83,848 | 1× | 9,897 | 43,969 | 105,543 | 0 | 0 | 0 | 632 | 141,668 | sender late |
| 167,698 | 167,697 | 2× | 8,720 | 40,493 | 219,233 | 0 | 0 | 0 | 1,566 | 107,150 | sender late |
| 419,244 | 419,244 | 5× | 7,340 | 37,944 | 2,495,659 | 0 | 0 | 0 | 3,541 | 228,291 | sender late |
| 838,489 | 838,489 | 10× | 6,888 | 56,386 | 4,050,418 | 0 | 0 | 0 | 7,292 | 764,196 | sender late |
| 2,096,222 | 2,096,222 | 25× | 7,189 | 1,786,340 | 4,233,402 | 0 | 0 | 0 | 15,828 | 2,791,047 | sender late |
| 4,192,445 | 4,192,421 | 50× | 86,055 | 76,417,052 | 79,388,532 | 9,151 | 0 | 0 | 65,527 | 144,859 | **lossy** |
| 8,384,890 | 8,384,842 | 100× | 5,464,868 | 11,760,179 | 12,668,929 | 25,375 | 0 | 124 | 65,536 | 820,899 | **lossy** |
| 16,769,780 | 16,769,078 | 200× | 5,224,894 | 10,303,592 | 11,306,112 | 58,142 | 0 | 1 | 65,536 | 371,609 | **lossy** |
| 33,539,560 | 28,511,047 | 400× | 5,596,801 | 9,449,978 | 10,217,484 | 78,259 | 0 | 1 | 65,536 | 31,211,752 | **lossy** |

## The two annotations

| | |
|---|---|
| max sustainable rate | 2,096,222 msg/s achieved (2,096,222 offered, 25× real time) |
| ...at which | p50 7,189 ns, p99 1,786,340 ns, p99.9 4,233,402 ns |
| knee | 2,096,222 msg/s (25× real time), p99 1,786,340 ns against a 40,493 ns baseline |
| cliff | 4,192,445 msg/s (50×) — 9,151 ring-full, 0 kernel, 0 mid-block |

<!-- generated:end -->

## What the shape says

Two observations survive the caveats, because they are about shape rather than
magnitude.

**p50 falls as the offered rate rises, then flattens.** That is not the
pipeline getting faster. At low rates the two threads are descheduled between
messages and every message pays a wake-up; the arrival stamp is taken once per
`recvmmsg` batch, and at one times real time a batch is usually one packet. As
the rate climbs the pipeline stays hot and the batch fills, so the per-message
cost falls toward the work itself. A single-threaded benchmark cannot show this
at all, which is most of why phase 10 exists.

**The tail leaves long before the drops do.** p99.9 climbs by orders of
magnitude while p50 is still flat and while both drop counters are still zero.
The knee is a queueing phenomenon and the cliff is a capacity one, and the gap
between them is the region where a system is already failing its latency
budget while every counter it keeps still reads clean. That gap is the argument
for measuring the distribution rather than a mean, and for keeping the bucket
histogram beside the curve.

## A hypothesis that died

The sweep reports the sender missing its schedule at *every* rate, and the
lateness is worse at low rates than at high ones — 33 ms at one times real time
against 600 µs at twenty times. That is backwards from what a load generator
usually does, and it had an obvious mechanism: at low rates the sender must
sleep between packets, and `wire_to_book`'s book thread spins on the ring
without pausing or yielding, so on two cores the spin owns one and the sender's
`nanosleep` cannot get a core back to wake up on time.

The first measurement agreed emphatically. Sender p99.9 lateness with nothing
else running: 26 µs. With the pipeline consuming: 14,277,397 ns. **539×.** A
bounded spin with a yield was written to fix it.

It fixed nothing, because there was nothing there. Best of five runs per
configuration:

| | sender alone | sender + consumer |
|---|---:|---:|
| best of 5 | 811,999 ns | 562,005 ns |
| range across repeats | 812 µs – 40 ms | 562 µs – 45 ms |

Indistinguishable, and nominally *better* with the consumer running. The
original pair was one sample from each side of a distribution that spans forty
milliseconds; the 539× was noise wearing a mechanism's clothes. The scheduler
jitter in this container is simply larger than anything being measured — which
is what `NO RATE QUALIFIED` already said, in one line, before any of this.

The yield was reverted and the spin left unbounded, which is the right design
for a consumer that owns a core. The episode is recorded rather than deleted
for two reasons: it is the second time in this phase that a single-sample
measurement produced a confident and wrong mechanism, and the first thing a
reader of the sweep output will reach for is the same hypothesis.

<!-- generated:overlap:begin -->

## 10.8 — decompressing on its own thread

The prediction was that the ceiling is arithmetic: decompression costs D, the book costs B, `parse()` alternates between them on one thread, so the sequential path pays D + B and the overlapped path cannot beat max(D, B). Available speedup (D + B) / max(D, B), at most 2×.

**The measurement went straight through that ceiling** — at every chunk size, by 24% to 37%. A pipeline cannot beat max(D, B), so the fault is in the decomposition, and the model's hidden assumption is the culprit: that the work is *invariant under the split*. It is not.

| | |
|---|---:|
| feed | 5,034,007 messages |
| D — decompress, frame, length-check, build nothing | 0.48 s |
| B — the book, by subtraction (T_seq − D) | 1.53 s |
| D + B — the single-threaded path | 2.01 s |
| model ceiling (D + B) / max(D, B) | 1.31× |
| larger half | book |
| runs per configuration | 5 (best of) |

### Why the ceiling leaks, measured

`gzread` reads an uncompressed file transparently, so the same binaries run on the same bytes with inflate taken out of the picture. That separates *what the book costs* from *what the book appears to cost while interleaved with inflate*.

| | |
|---|---:|
| frame only, no inflate | 0.12 s |
| frame + book, no inflate | 1.57 s |
| **B isolated** | **1.45 s** |
| subtraction overstates the book by | 6% |

Two effects, both real, and neither one is overlap:

1. **Inflate and the book contend.** B by subtraction is 1.53 s; the book's isolated cost is 1.45 s. zlib's 32 KB window and the book's ref map do not fit in the same cache together, so interleaving them on one core costs more than running either alone.

2. **The split moves work off the consumer.** Framing alone is 0.12 s for 5,034,007 messages — two `gzread` calls and a vector resize each. In the pipeline the producer absorbs all of it and the consumer walks a contiguous chunk instead. That is a cheaper inner loop, not overlap, and it lands in the same number.

Stall columns are **poll counts, not time**, and are not comparable across the two sides: a consumer's empty poll is a load and a compare, while a producer's full poll goes through `writable()`, which refreshes the consumer's cache line. Which half is slower is settled above, by time.

| chunk | wall clock | speedup | % of ceiling | producer polls | consumer polls | chunks |
|---:|---:|---:|---:|---:|---:|---:|
| 64 KB | 1.11 s | 1.81× | 138% | 669,141,430 | 36,823,191 | 2,310 |
| 256 KB | 1.10 s | 1.83× | 139% | 492,213,336 | 6,869,883 | 578 |
| 1024 KB | 1.05 s | 1.91× | 146% | 465,208,847 | 6,800,350 | 145 |

### The predictions, graded

| | predicted | measured | verdict |
|---|---|---|:--|
| P1 the ceiling is arithmetic | measured speedup ≤ (D+B)/max(D,B) | ceiling 1.31×, best 1.91× (146%) | **falsified — the model assumed the work is invariant under the split, and it is not** |
| P2 speedup here | 1.2–1.7× | 1.91× at 1024 KB | **falsified — above the range** |
| P3 falsification | refuted if overlapped ≥ 0.95× sequential | 0.52× sequential | **holds** |
| P4 chunk size | FLAT | 5.7% spread across 64 KB / 256 KB / 1 MB, monotonic | **undecided** — spread is within the noise of this machine, but bigger chunks were faster at every size and in both runs of the sweep. A real effect too small to separate here |
| P5 which side stalls | producer more often (B > D) | producer 669,141,430, consumer 36,823,191 | **withdrawn — poll counts are not comparable across the sides; see above** |

<!-- generated:overlap:end -->

## What 10.8 does and does not close

Phase 9 reported a full trading day end to end and then reported, honestly and
repeatedly, that a large part of that number was gzip. Those two costs were
strictly sequential: `parse()` called `gzread`, then the handler, then `gzread`
again, on one thread. The reader thread puts them on two, through the same ring
phase 10 already built — a different slot type, a chunk instead of a message,
because one publish per 64 KB is a rounding error where one publish per 40 bytes
would not be.

What it does not close is the *decompression* cost itself. Overlap hides the
smaller half behind the larger one; it does not make either faster, and the
ceiling above says exactly how much is on the table. A feed that is
decompression-bound has almost nothing to gain and the table will say so.

**Two bugs, and only one of them was findable by comparing books.**

The first hung. The producer's fill limit was `c.len + 2 + 65535 > ChunkBytes`,
which with the default 64 KB chunk is `c.len + 65537 > 65536` — true on the
first iteration and every one after. It broke out before reading a byte,
published nothing, never reached EOF, and spun forever. The reservation was
larger than the buffer it was reserving from. It was found by running a tool and
waiting, which is the slowest way to find anything.

The second was a heap-buffer-overflow, and no book comparison could have caught
it. A message that does not fit alongside what is already staged is carried
whole to the next chunk — and the carry was `memcpy`'d in at the top of that
chunk with no size check. A 902-byte message behind a 50-byte one, with a
256-byte chunk, wrote 902 bytes into 256. ASan caught it on the first run of the
new test. It could never fire on real data, because ITCH messages top out at 50
bytes and the default chunk is 64 KB — which is exactly why it needed a test
that chooses hostile chunk sizes rather than realistic ones. The check now
happens on the read, where whether a message fits is a property of the message
and the chunk size rather than of what happens to be staged.

`tests/test_reader_thread.cpp` compares the **message stream**, not the book:
same types, same bytes, same order, same count, same failures on the same
malformed input, at chunk sizes of 512 B, 1 KB and 4 KB where almost every
message straddles a boundary. Two paths can agree on a book while disagreeing
about which messages they saw.

## Figures

- `docs/figures/rate-latency.svg` — p50/p99/p99.9 against offered rate,
  log–log, with the knee and the max sustainable rate drawn as vertical rules.
- `docs/figures/wire-to-book-hist.svg` — the distribution at the max
  sustainable rate. Two pipelines can share a p50 and a p99 and have completely
  different shapes; the second is a mechanism you can go and find.

Both regenerate from the committed JSON:

```
python3 python/analysis/rate_latency.py validation/rate-sweep.json \
    --svg docs/figures/rate-latency.svg \
    --hist-svg docs/figures/wire-to-book-hist.svg
```

## Reproducing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python3 bench/rate-sweep.py --build build --out validation/rate-sweep.json \
    --repeats 5 --multipliers 1,2,5,10,25,50,100,200
python3 scripts/phase10-report.py
```

On a host with cores to spare, pin the two threads with `--cpu-recv` and
`--cpu-book` on `wire_to_book` and run `tools/tsc_offset` first. If the offset
is not measurable, neither is the latency.
