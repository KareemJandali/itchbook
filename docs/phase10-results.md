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

The tables below come from a **pinned run** on an i7-11700K — eight physical
cores, sixteen logical — with three threads on three distinct CPUs, an invariant
TSC, and a cross-core offset small enough to be a rounding error against the
latency being measured. The bound is in the generated table below rather than
typed here, because it changes with every run: the artifact committed at HEAD
reports 47 ns and the one the table below reads reports a different value, and a
figure hand-copied into prose is a figure that is wrong by the next run.

The topology is real and can be asked for. `/sys/devices/system/cpu/cpu*/topology/`
is fully populated on this host: eight `core_id`s, siblings paired 0-1, 2-3, …,
14-15, and `lscpu -e` agrees. So `--cores "11 13 15"` is CPU 11 on core 5, CPU 13
on core 6 and CPU 15 on core 7 — three distinct physical cores, which is what
`docs/phase10-methodology.md` §4 asks for.

Two things worth recording about getting there, because both are traps. The
script's own default would have been wrong: `nproc` returns **13** here, not 16,
because `isolcpus=13,14,15` removes those CPUs from the shell's affinity mask,
so `scripts/pinned-run.sh`'s last-three rule yields 10, 11, 12 — and 10 and 11
are siblings of core 5. And the isolated set is worse than the default: 13, 14
and 15 span only cores 6 and 7, so no three-way distinct-core assignment exists
inside it at all. A run pinned to 13/14/15 puts two of the three threads on one
physical core while reporting all three as pinned.

That run was made, and it is preserved as a null result: pinned to 13/14/15 with
the book and the sender sharing core 7, the sweep produced **the same knee (10×),
the same cliff (25×) and the same max sustainable rate (838,489 msg/s)** as the
distinct-core run. On this host the SMT collision is not what limits anything —
see the CPU-availability measurement below, which is.

**The clock is settled.** `tsc_offset` pins two threads to two separate CPUs
and ping-pongs 200,000 samples in each direction. The offset estimate comes
back smaller than the method can resolve — bounded, not distinguishable
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
   had three distinct physical cores to themselves and it made no difference to
   lateness, and neither did giving two of them the same core.
2. **Pinning is real here**, unlike before. `pinned` reads true and `cpus`
   records 11/13/15 — cores 5, 6 and 7. The script's default on this host would
   have been 10/11/12, two of which share core 5, while reporting all three as
   pinned. Nothing in the harness reads the sibling map, so the placement is the
   operator's to get right; `--cores` is how.
3. **The arrival and completion stamps come from the same clock family**, and
   the offset between the two CPUs reading it is bounded well under the
   microsecond the latency is measured in — the figure is in the generated
   table. Their difference means something now.

What remains is a load generator that can hold a sub-10 µs schedule.
`scripts/pinned-run.sh` is the run; it needs a different host.

**`isolcpus` IS available under WSL2, and it does not help.** An earlier version
of this section said it was unavailable. It is not: `kernelCommandLine =
isolcpus=13,14,15` in `%USERPROFILE%\.wslconfig` puts it in `/proc/cmdline`, and
`/sys/devices/system/cpu/isolated` then reads `13-15`. What it buys is measured
below, and the answer is nothing.

**This machine cannot hold a CPU, and that is the obstacle.** `tools/cpu_jitter`
exists to answer the question underneath every latency figure in the phase: a
thread pinned to a CPU, asking for nothing, reading a monotonic clock in a loop.
A gap between consecutive reads is time the thread was not executing, because
nothing in the loop can take longer than a clock read. Four CPUs at once, from
one barrier, 20 s, on an otherwise idle box — two isolated and two not, all four
on distinct physical cores (`validation/cpu-jitter.json`):

| CPU | isolated | gaps/s > 10 µs | gaps/s > 1 ms | worst gap | wall lost to gaps > 100 µs |
|---:|:--|---:|---:|---:|---:|
| 5 | no | 1,306 | 3.3 | 9.85 ms | 2.34% |
| 11 | no | 1,360 | 3.5 | 10.21 ms | 2.91% |
| 13 | **yes** | 1,352 | 4.6 | 10.56 ms | 3.50% |
| 15 | **yes** | 1,355 | 4.4 | 11.09 ms | 3.56% |

An idle, pinned CPU is off-CPU for longer than 10 µs about **1,343 times a
second**, and the isolated CPUs are marginally *worse* than the ones in the
general pool, not better. `isolcpus` removes a CPU from the guest scheduler's
load-balancing mask and has no representation on the host side at all.

**And the guest does not know.** Over the same 20 seconds the kernel credited
each thread with 19,998.7 ms of CPU against 20,000.0 ms of wall — 1.2–2.1 ms
unaccounted — and reported **5 involuntary context switches** against roughly
27,000 gaps longer than 10 µs. `/proc/pressure/cpu` reads 0.00 and `/proc/stat`
steal reads 0.

**And it happens to all four at once.** The ten worst gaps on every CPU cluster
in the same ~300 ms window (t ≈ 16.75–17.06 s), each around 10 ms, on isolated
and non-isolated CPUs alike. That is not per-CPU scheduling; it is the whole VM
being descheduled underneath a guest with no way to observe it. No in-guest
setting reaches that, which is why the remedy is a different host rather than a
different flag.

**Pinning alone is not enough, and this has now been measured three times.** In
the container, best-of-five sender lateness went from 562 µs unpinned to 39 µs
pinned — a 14× improvement that still missed the 10 µs bar by 4×. On this host,
with three CPUs to itself, the bar is missed at every rate anyway.

The third measurement is the one the methodology asks for first and that had
never been run: **the sender alone, against a port nothing is listening on**, at
the full 5,032,462-message feed, with RT bandwidth throttling lifted so the
scheduling class is the only variable. Recorded under
`validation/sender-qualification/`:

| condition | p50 | p99 | **p99.9** | worst |
|---|---:|---:|---:|---:|
| CPU 14 (isolated), SCHED_OTHER | 30 | 34,017 | 241,385 | 2,840,577 |
| CPU 14, SCHED_FIFO 80 | 30 | 15,791 | **115,103** | 1,644,831 |
| CPU 5 (**not** isolated), SCHED_FIFO 80 | 30 | 22,490 | **106,002** | 1,338,978 |
| CPU 14, SCHED_FIFO 80, at 1× real time | 40 | 28,758 | **294,423** | 4,319,590 |
| CPU 14, SCHED_FIFO 80, spin margin 100 µs | 41 | 78,080 | 256,050 | 3,690,585 |

Nanoseconds. The bar is 10,000. The best case is **10.6× over it** with no
receiver, no book and nothing else running; at 1× real time — which is what the
first open done-item requires — it is **29× over**. This machine is disqualified
by the methodology's own entrance exam, and the sweep below is therefore a test
of the harness rather than a measurement of the pipeline.

Two things that table settles in passing. Lowering the spin margin makes the
tail *worse*, not better — p99 15,791 → 78,080 ns at 100 µs — which is what the
tool's old advice to *raise* it predicted; the advice was incomplete rather than
backwards, because it never said that raising the margin raises the duty cycle
and walks a SCHED_FIFO sender into RT bandwidth throttling. Nothing here tests
above 500 µs, so the upper direction remains untested on this host and the
methodology's own sweep (100/500/2000 µs → 96/27/21 µs) is the only evidence for
it. And every row records `memory_locked: false`: with `ulimit -l` at 64 KB,
`mlockall` fails once the preloaded feed is any real size, which
`setcap cap_sys_nice` alone does not fix — it needs `cap_ipc_lock` too.

The CPU, isolation and rate columns above are the `taskset` and `--rate`
invocation, not fields in the JSON: `mold_replay_udp` records rate, lateness and
`realtime{}` and nothing about placement. From the files alone, the scheduling
class and the 1× rate are checkable; the rest is the run description.

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
socket buffer can overflow before the ring does. A sustainable rate requires zero
of both, and `/proc/net/udp` being unreadable is reported as UNKNOWN rather than
as zero — "no drops" and "this platform cannot tell you" are different claims.

<!-- generated:begin -->

> **NOT QUOTABLE.**
>
> - The load generator missed its schedule at every rate: p99.9 lateness exceeded 10,000 ns at all 9 rungs, which is the same order as the latency this run exists to measure. The curve, the knee, the cliff and the max sustainable rate below therefore cannot be attributed to this pipeline.
>
> Everything below is printed in full because deleting it would hide the evidence. None of it may be quoted.

## What was run

| | |
|---|---:|
| feed | 5,032,462 messages over 60.0 s of session |
| one times real time | 83,849 msg/s |
| ring | 65,536 slots |
| clock | rdtsc / rdtscp (per-core counter) |
| cross-core clock offset | **bounded under 49 ns**, not measured — the estimate is smaller than the method can resolve (rdtsc / rdtscp (per-core counter)) |
| threads pinned | yes |
| runs per rate | 5 (best of) |
| rates on the ladder | 9 |
| rates where the sender held its schedule | 0 of 9 |

## The curve

Kernel drops and ring drops are separate columns, never a sum. On loopback either can overflow first — which one does is a property of the ring size against the socket buffer, not of loopback — so a table that added them could not tell a pipeline that is drowning from one whose water came in upstream.

Offered is what the sender was told to send; achieved is what it managed. They are the same number only while it keeps its schedule, and the pipeline can only have absorbed the second one.

| offered | achieved | × real time | p50 ns | p99 ns | p99.9 ns | ring-full | kernel | mid-block | peak occupancy | sender p99.9 late | verdict |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 83,849 | 83,848 | 1× | 14,163 | 76,175 | 298,728 | 0 | 0 | 0 | 883 | 136,073 | sender late |
| 167,698 | 167,697 | 2× | 18,330 | 122,763 | 2,662,667 | 0 | 0 | 0 | 3,089 | 954,843 | sender late |
| 419,244 | 419,244 | 5× | 17,560 | 201,354 | 8,519,856 | 0 | 0 | 0 | 8,493 | 3,569,883 | sender late |
| 838,489 | 838,489 | 10× | 15,803 | 1,700,240 | 8,555,158 | 0 | 0 | 0 | 12,821 | 5,498,893 | sender late |
| 2,096,222 | 2,096,221 | 25× | 578,553 | 113,877,703 | 126,436,251 | 12,780 | 0 | 1 | 65,536 | 8,767,867 | **lossy**, sender late |
| 4,192,445 | 4,192,410 | 50× | 12,673,989 | 21,787,653 | 24,274,673 | 30,245 | 0 | 2,719 | 65,536 | 289,648 | **lossy**, sender late |
| 8,384,890 | 8,384,460 | 100× | 12,827,247 | 19,517,977 | 20,232,600 | 65,901 | 0 | 0 | 65,536 | 354,116 | **lossy**, sender late |
| 16,769,780 | 15,717,107 | 200× | 15,604,404 | 26,768,048 | 29,531,689 | 88,396 | 0 | 43 | 65,536 | 33,219,262 | **lossy**, sender late |
| 33,539,560 | 17,426,914 | 400× | 13,331,213 | 20,301,118 | 21,840,023 | 86,964 | 0 | 0 | 65,536 | 138,790,615 | **lossy**, sender late |

## The two annotations

| | |
|---|---|
| max sustainable rate | 838,489 msg/s achieved (838,489 offered, 10× real time) |
| ...at which | p50 15,803 ns, p99 1,700,240 ns, p99.9 8,555,158 ns |
| knee | 838,489 msg/s (10× real time), p99 1,700,240 ns against a 122,763 ns baseline |
| cliff | 2,096,222 msg/s (25×) — 12,780 ring-full, 0 kernel, 1 mid-block |

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

**The measurement went straight through that ceiling** — at every chunk size, by 49% to 63%. A pipeline cannot beat max(D, B), so the fault is in the decomposition, and the model's hidden assumption is the culprit: that the work is *invariant under the split*. It is not.

| | |
|---|---:|
| feed | 5,034,007 messages |
| D — decompress, frame, length-check, build nothing | 0.58 s |
| B — the book, by subtraction (T_seq − D) | 2.71 s |
| D + B — the single-threaded path | 3.29 s |
| model ceiling (D + B) / max(D, B) | 1.21× |
| larger half | book |
| runs per configuration | 5 (best of) |

### Why the ceiling leaks, measured

`gzread` reads an uncompressed file transparently, so the same binaries run on the same bytes with inflate taken out of the picture. That separates *what the book costs* from *what the book appears to cost while interleaved with inflate*.

| | |
|---|---:|
| frame only, no inflate | 0.15 s |
| frame + book, no inflate | 2.44 s |
| **B isolated** | **2.29 s** |
| subtraction overstates the book by | 18% |

Two effects, both real, and neither one is overlap:

1. **Inflate and the book contend.** B by subtraction is 2.71 s; the book's isolated cost is 2.29 s. zlib's 32 KB window and the book's ref map do not fit in the same cache together, so interleaving them on one core costs more than running either alone.

2. **The split moves work off the consumer.** Framing alone is 0.15 s for 5,034,007 messages — two `gzread` calls and a vector resize each. In the pipeline the producer absorbs all of it and the consumer walks a contiguous chunk instead. That is a cheaper inner loop, not overlap, and it lands in the same number.

Stall columns are **poll counts, not time**, and are not comparable across the two sides: a consumer's empty poll is a load and a compare, while a producer's full poll goes through `writable()`, which refreshes the consumer's cache line. Which half is slower is settled above, by time.

| chunk | wall clock | speedup | % of ceiling | producer polls | consumer polls | chunks |
|---:|---:|---:|---:|---:|---:|---:|
| 64 KB | 1.79 s | 1.84× | 151% | 1,153,663,874 | 28,776,560 | 2,310 |
| 256 KB | 1.82 s | 1.81× | 149% | 1,178,038,059 | 7,024,499 | 578 |
| 1024 KB | 1.66 s | 1.98× | 163% | 842,985,317 | 6,933,801 | 145 |

### The predictions, graded

| | predicted | measured | verdict |
|---|---|---|:--|
| P1 the ceiling is arithmetic | measured speedup ≤ (D+B)/max(D,B) | ceiling 1.21×, best 1.98× (163%) | **falsified — the model assumed the work is invariant under the split, and it is not** |
| P2 speedup here | 1.2–1.7× | 1.98× at 1024 KB | **falsified — above the range** |
| P3 falsification | refuted if overlapped ≥ 0.95× sequential | 0.50× sequential | **holds** |
| P4 chunk size | FLAT | 9.6% spread across 64 KB / 256 KB / 1 MB, not ordered | **kept — flat** |
| P5 which side stalls | producer more often (B > D) | producer 1,153,663,874, consumer 28,776,560 | **withdrawn — poll counts are not comparable across the sides; see above** |

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
