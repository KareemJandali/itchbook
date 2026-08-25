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

## Where these numbers come from

**Bare metal.** An i7-11700K — eight physical cores, sixteen logical — booted
from a USB into an Ubuntu 26.04 live session, with the pipeline pinned to CPUs
7, 6 and 5, which on this host are three distinct physical cores. The run was
made as root, so `SCHED_FIFO` and `mlockall` were granted rather than requested
and denied. `/proc/cpuinfo` carries no `hypervisor` flag; `tools/cpu_jitter`
confirms what that is worth below.

That sentence is the whole difference between this document and the three
versions of it that came before. Every earlier attempt was made in a WSL2 guest,
and every one of them reported `NO RATE QUALIFIED` — the load generator missed
its own schedule at every rate on the ladder, so the numbers described the
generator. Those runs are kept in the comparison below, because two machines
measured the same way say more than either alone.

## The machine passed its entrance exam

`docs/phase10-methodology.md` §1 asks for one thing before anything else: run
the sender alone, against a port nothing is listening on, and see whether it can
hold a schedule. If it cannot, nothing measured afterwards is about the
pipeline. Recorded under `validation/sender-qualification/`:

| host | rate | p50 | p99 | **p99.9** | worst |
|---|---:|---:|---:|---:|---:|
| **bare metal** | 200,000 msg/s | 36 | 45 | **46** | 19,238 |
| **bare metal** | 83,849 msg/s (1×) | 38 | 49 | **810** | 9,109 |
| WSL2 | 200,000 msg/s | 30 | 22,490 | 106,002 | 1,338,978 |
| WSL2 | 83,849 msg/s (1×) | 40 | 28,758 | 294,423 | 4,319,590 |

Nanoseconds; the bar is 10,000 at p99.9. Bare metal clears it by **217×** at
200,000 msg/s and by **12×** at one times real time. The same binary on the same
silicon under a hypervisor missed it by 10.6× and 29×. Nothing about the program
changed between those rows — only what was underneath it.

Both bare-metal rows record `scheduler_granted: true`, `memory_locked: true` and
zero send errors, and both achieved exactly the rate they were offered.

## What the hypervisor was actually costing

`tools/cpu_jitter` asks the question underneath every latency figure in this
phase: a thread pinned to a CPU, asking for nothing, reading a monotonic clock
in a loop. A gap between consecutive reads is time the thread was **not
executing**, because nothing in the loop can take longer than a clock read.

| host | CPU | gaps/s > 10 µs | gaps/s > 100 µs | gaps/s > 1 ms | worst gap |
|---|---:|---:|---:|---:|---:|
| **bare metal** | 7 | 30 | **0** | **0** | 24,732 ns |
| **bare metal** | 6 | 30 | **0** | **0** | 24,730 ns |
| **bare metal** | 5 | 32 | **0** | **0** | 43,298 ns |
| WSL2 | 13 (isolated) | 1,352 | 95 | 4.6 | 10,558,100 ns |
| WSL2 | 15 (isolated) | 1,355 | 96 | 4.4 | 11,087,271 ns |
| WSL2 | 5 (not isolated) | 1,306 | 79 | 3.3 | 9,850,366 ns |

`validation/cpu-jitter-baremetal.json` and `validation/cpu-jitter.json`. Roughly
**45× fewer** interruptions over 10 µs, **none at all** over 100 µs against ~90 a
second, and a worst case **250× smaller** — 43 µs against 11 ms.

One note on those two files. Both carry `holds_a_cpu: false`, and for the
bare-metal one that field is wrong: the binary that wrote it gated on *literally
zero* gaps over 10 µs, a bar no real machine clears. The tool now gates on gaps
over 100 µs — long enough to move a microsecond-scale p99.9, where a 43 µs blip
at 30 a second is not — and recomputing from the numbers those same files record
gives **true** for bare metal and false for WSL2. The gap counts in the table
above are what the tool measured and are unaffected; only the verdict field
predates the fix.

Three things the WSL2 side of that table settles, and they are worth keeping
because each was a plausible theory that turned out to be wrong:

**`isolcpus` bought nothing.** It is available under WSL2 —
`kernelCommandLine = isolcpus=13,14,15` in `.wslconfig` puts it in
`/proc/cmdline` — and the isolated CPUs came out marginally *worse* than the
ones left in the general pool. It removes a CPU from the guest scheduler's
load-balancing mask and has no representation on the host side at all.

**The guest could not see what it was losing.** Over 20 seconds the kernel
credited each thread with 19,998.7 ms of CPU against 20,000.0 ms of wall and
reported **5 involuntary context switches** against roughly 27,000 gaps longer
than 10 µs. `/proc/pressure/cpu` read 0.00 and `/proc/stat` steal read 0. A
guest losing the CPU underneath the kernel cannot diagnose itself from either.

**It happened to every CPU at once.** The ten worst gaps on all four clustered
in the same ~300 ms window, each around 10 ms, isolated and non-isolated alike.
That is the whole VM being descheduled, not per-CPU scheduling — which is why no
in-guest setting reached it and the remedy was a different host rather than a
different flag.

`nproc` is worth one warning. It reports the calling shell's *affinity mask*, so
under `isolcpus=13,14,15` it returned **13**, not 16 — and `pinned-run.sh`'s
last-three default therefore proposed CPUs that were SMT siblings. The script now
reads `core_id` from sysfs and says so when two of the three share a core.

## The clock

`tsc_offset` pins two threads to two separate CPUs and ping-pongs 200,000
samples in each direction. The offset comes back **smaller than the method can
resolve** — bounded rather than measured, which is what healthy hardware gives —
against a wire-to-book p50 in the microseconds. The figures are in the generated
table below rather than typed here, because they move run to run: four
successive runs on this hardware reported 47, 48, 58 and 73 ns of resolution. A
figure hand-copied into prose is a figure that is wrong by the next run.

## One thing these numbers do not explain

The p99.9 column climbs to 1.5 ms at 5× and 3.4 ms at 10× on rows that are
otherwise clean — no drops, sender holding its schedule to under a microsecond,
peak ring occupancy of 2,318 and 5,107 slots out of 65,536. There is not enough
queue at those rates for queueing to explain it, and `cpu_jitter` measured a
worst gap of 43 µs on the same machine minutes earlier, so it is not obviously
the scheduler either.

It does not invalidate the run — the sender qualified, the drop accounting is
sound, and p50 and p99 are stable across a 25-fold rate change. But the far tail
below the knee is not yet accounted for, and it should be understood before any
of it is quoted as a property of the pipeline rather than of the measurement.


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

> **Partly quotable.** The sender held its schedule at 7 of 9 rates; the rows whose verdict includes *sender late* below measure the generator as much as the pipeline.

## What was run

| | |
|---|---:|
| feed | 5,032,462 messages over 60.0 s of session |
| one times real time | 83,849 msg/s |
| ring | 65,536 slots |
| clock | rdtsc / rdtscp (per-core counter) |
| cross-core clock offset | **bounded under 73 ns**, not measured — the estimate is smaller than the method can resolve (rdtsc / rdtscp (per-core counter)) |
| threads pinned | yes |
| runs per rate | 5 (best of) |
| rates on the ladder | 9 |
| rates where the sender held its schedule | 7 of 9 |

## The curve

Kernel drops and ring drops are separate columns, never a sum. On loopback either can overflow first — which one does is a property of the ring size against the socket buffer, not of loopback — so a table that added them could not tell a pipeline that is drowning from one whose water came in upstream.

Offered is what the sender was told to send; achieved is what it managed. They are the same number only while it keeps its schedule, and the pipeline can only have absorbed the second one.

| offered | achieved | × real time | p50 ns | p99 ns | p99.9 ns | ring-full | kernel | mid-block | peak occupancy | sender p99.9 late | verdict |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|
| 83,849 | 83,848 | 1× | 6,189 | 22,696 | 27,885 | 0 | 0 | 0 | 426 | 715 | clean |
| 167,698 | 167,697 | 2× | 5,745 | 22,802 | 32,106 | 0 | 0 | 0 | 850 | 563 | clean |
| 419,244 | 419,244 | 5× | 5,296 | 22,177 | 1,512,661 | 0 | 0 | 0 | 2,318 | 486 | clean |
| 838,489 | 838,489 | 10× | 5,272 | 22,201 | 3,434,200 | 0 | 0 | 0 | 5,107 | 615 | clean |
| 2,096,222 | 2,096,222 | 25× | 5,290 | 1,081,195 | 4,606,539 | 0 | 0 | 0 | 16,145 | 564 | clean |
| 4,192,445 | 4,192,444 | 50× | 5,789 | 106,403,850 | 116,071,334 | 10,516 | 0 | 0 | 65,536 | 3,184 | **lossy** |
| 8,384,890 | 8,384,888 | 100× | 4,874,644 | 10,423,107 | 11,692,954 | 14,657 | 0 | 124 | 65,536 | 7,946 | **lossy** |
| 16,769,780 | 16,769,702 | 200× | 5,023,418 | 7,392,616 | 7,583,896 | 53,362 | 0 | 0 | 65,536 | 12,537 | **lossy**, sender late |
| 33,539,560 | 21,606,187 | 400× | 5,180,810 | 8,205,128 | 8,539,881 | 67,318 | 0 | 42 | 65,536 | 82,842,602 | **lossy**, sender late |

## The two annotations

| | |
|---|---|
| max sustainable rate | 2,096,222 msg/s achieved (2,096,222 offered, 25× real time) |
| ...at which | p50 5,290 ns, p99 1,081,195 ns, p99.9 4,606,539 ns |
| knee | 2,096,222 msg/s (25× real time), p99 1,081,195 ns against a 22,696 ns baseline |
| cliff | 4,192,445 msg/s (50×) — 10,516 ring-full, 0 kernel, 0 mid-block |

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

**The measurement went straight through that ceiling** — at every chunk size, by 21% to 35%. A pipeline cannot beat max(D, B), so the fault is in the decomposition, and the model's hidden assumption is the culprit: that the work is *invariant under the split*. It is not.

| | |
|---|---:|
| feed | 5,034,007 messages |
| D — decompress, frame, length-check, build nothing | 0.64 s |
| B — the book, by subtraction (T_seq − D) | 1.36 s |
| D + B — the single-threaded path | 2.00 s |
| model ceiling (D + B) / max(D, B) | 1.47× |
| larger half | book |
| runs per configuration | 5 (best of) |

### Why the ceiling leaks, measured

`gzread` reads an uncompressed file transparently, so the same binaries run on the same bytes with inflate taken out of the picture. That separates *what the book costs* from *what the book appears to cost while interleaved with inflate*.

| | |
|---|---:|
| frame only, no inflate | 0.16 s |
| frame + book, no inflate | 1.48 s |
| **B isolated** | **1.32 s** |
| subtraction overstates the book by | 3% |

Two effects, both real, and neither one is overlap:

1. **Inflate and the book contend.** B by subtraction is 1.36 s; the book's isolated cost is 1.32 s. zlib's 32 KB window and the book's ref map do not fit in the same cache together, so interleaving them on one core costs more than running either alone.

2. **The split moves work off the consumer.** Framing alone is 0.16 s for 5,034,007 messages — two `gzread` calls and a vector resize each. In the pipeline the producer absorbs all of it and the consumer walks a contiguous chunk instead. That is a cheaper inner loop, not overlap, and it lands in the same number.

Stall columns are **poll counts, not time**, and are not comparable across the two sides: a consumer's empty poll is a load and a compare, while a producer's full poll goes through `writable()`, which refreshes the consumer's cache line. Which half is slower is settled above, by time.

| chunk | wall clock | speedup | % of ceiling | producer polls | consumer polls | chunks |
|---:|---:|---:|---:|---:|---:|---:|
| 64 KB | 1.12 s | 1.79× | 121% | 385,341,573 | 97,788,563 | 2,310 |
| 256 KB | 1.05 s | 1.90× | 130% | 229,349,781 | 10,985,265 | 578 |
| 1024 KB | 1.01 s | 1.98× | 135% | 193,920,716 | 8,252,146 | 145 |

### The predictions, graded

| | predicted | measured | verdict |
|---|---|---|:--|
| P1 the ceiling is arithmetic | measured speedup ≤ (D+B)/max(D,B) | ceiling 1.47×, best 1.98× (135%) | **falsified — the model assumed the work is invariant under the split, and it is not** |
| P2 speedup here | 1.2–1.7× | 1.98× at 1024 KB | **falsified — above the range** |
| P3 falsification | refuted if overlapped ≥ 0.95× sequential | 0.51× sequential | **holds** |
| P4 chunk size | FLAT | 10.9% spread across 64 KB / 256 KB / 1 MB, monotonic | **falsified — chunk size matters** |
| P5 which side stalls | producer more often (B > D) | producer 385,341,573, consumer 97,788,563 | **withdrawn — poll counts are not comparable across the sides; see above** |

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
