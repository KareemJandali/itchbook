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

The tables below were produced **in a two-core container, with neither thread
pinned, on a machine whose cross-core TSC offset `tsc_offset` reports as
UNMEASURABLE**. They are here because the machinery is worth showing working and
because a committed artifact is how this repository stops numbers from drifting.
They are not results.

Three specific reasons not to quote them:

1. **The sender could not hold its schedule.** `mold_replay_udp` measures its
   own lateness and reports it per rate. Where its p99.9 lateness is the same
   order as the latency being measured, the experiment measured the load
   generator — and the sender and the two pipeline threads are competing for two
   cores here, so it usually is.
2. **Unpinned.** Phase 4 measured 19.3% run-to-run variance on this repository's
   own benchmark without pinning. Nothing here is pinned, because macOS offers
   no thread affinity and this container has nowhere to pin to.
3. **The arrival stamp and the completion stamp are read on different cores.**
   Whether their difference means anything is the question `tools/tsc_offset.cpp`
   exists to answer, and on this hardware its answer is "cannot be determined".

The real figures need a pinned Linux host with a measurable cross-core offset.
When they exist, they replace the artifact and this document regenerates.

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
| feed | 252,101 messages over 60.1 s of session |
| one times real time | 4,196 msg/s |
| ring | 65,536 slots |
| clock | rdtsc / rdtscp (per-core counter) |
| threads pinned | **no** |
| runs per rate | 2 (best of) |
| rates on the ladder | 9 |
| rates where the sender held its schedule | 0 of 9 |

## The curve

Kernel drops and ring drops are separate columns, never a sum. On loopback the socket buffer overflows before the ring does, so a table that added them could not tell a pipeline that is drowning from one whose water came in upstream.

Offered is what the sender was told to send; achieved is what it managed. They are the same number only while it keeps its schedule, and the pipeline can only have absorbed the second one.

| offered | achieved | × real time | p50 ns | p99 ns | p99.9 ns | ring-full | kernel | mid-block | peak occupancy | sender p99.9 late | verdict |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 4,196 | 4,195 | 1× | 29,495 | 87,862 | 189,312 | 0 | 0 | 0 | 257 | 32,973,504 | sender late |
| 20,980 | 20,979 | 5× | 14,632 | 62,336 | 730,785 | 0 | 0 | 0 | 841 | 26,813,468 | sender late |
| 83,918 | 83,918 | 20× | 11,755 | 70,949 | 5,119,791 | 0 | 0 | 0 | 739 | 600,048 | sender late |
| 209,795 | 209,794 | 50× | 10,685 | 249,562 | 835,897 | 0 | 0 | 0 | 1,040 | 83,761 | sender late |
| 419,590 | 419,579 | 100× | 13,016 | 11,430,918 | 15,842,025 | 0 | 0 | 0 | 7,533 | 120,658 | sender late |
| 839,180 | 839,132 | 200× | 9,427 | 830,228 | 1,393,779 | 0 | 0 | 0 | 3,640 | 154,425 | sender late |
| 1,678,360 | 1,678,344 | 400× | 7,812 | 12,780,676 | 13,552,185 | 0 | 0 | 0 | 28,539 | 194,265 | sender late |
| 3,356,720 | 3,356,066 | 800× | 25,287 | 10,553,152 | 10,800,259 | 0 | 0 | 0 | 51,296 | 127,473 | sender late |
| 6,713,440 | 5,820,443 | 1600× | 5,245,600 | 11,471,745 | 11,528,928 | 947 | 0 | 0 | 65,509 | 6,969,669 | **lossy** |

## The two annotations

| | |
|---|---|
| max sustainable rate | 3,356,066 msg/s achieved (3,356,720 offered, 800× real time) |
| ...at which | p50 25,287 ns, p99 10,553,152 ns, p99.9 10,800,259 ns |
| knee | 209,795 msg/s (50× real time), p99 249,562 ns against a 70,949 ns baseline |
| cliff | 6,713,440 msg/s (1600×) — 947 ring-full, 0 kernel, 0 mid-block |

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
