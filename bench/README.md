# Phase 4 — performance

Baseline first, then one change at a time, each measured. "It got faster" is not
a result.

Reproduce:

```bash
python3 python/make_bench_feed.py data/raw/bench.gz --messages 1000000
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python3 bench/compare.py ./build/book_bench data/raw/bench.gz
```

## The measurement mistake that came first

The first round of results was wrong, and the way it was wrong is worth keeping.

Unpinned on this machine, `book_bench` varies by **~19% between identical
invocations**. Measuring the baseline, making a change, and measuring again
produced a convincing **12% "speedup" from a change that does nothing** — the
machine had simply drifted between the two runs.

Two fixes, both now enforced by `bench/compare.py`:

| | run-to-run spread |
|---|---|
| unpinned | 19.3% |
| `taskset -c 2` | **2.0%** |

and **interleave the variants** — A, B, A, B, … never all of A then all of B,
so drift cannot be attributed to the change under test. Anything under ~5% on
this hardware is not a result.

Re-measured properly, the two changes the build plan predicted would win both
came out flat. The one that mattered was not on the list.

## Baseline

1,000,000 messages, mix taken from the validated MSFT 2019-12-30 day (47% `A`,
45% `D`, 4.5% `U`, 2.9% `E`). Intel Xeon @ 2.10GHz, invariant TSC, `-O3`.

**Which artifact backs which number.** The table below is `compare.py`'s output:
the **median of nine interleaved, pinned rounds**, for the reason the section
above gives — a single run on this machine varies by 19% unpinned and ~2%
pinned, so one measurement is not a number. `baseline.json` and `after.json`
beside this file are something else: single `book_bench --json` runs, kept
because they carry the full per-message-type breakdown that the summary table
does not. They will not equal the table, and should not — a single sample and a
median of nine are different statistics of the same thing. Where they differ
most is the tail, which is exactly where a single run is least trustworthy:
`baseline.json` records a worst message of 51,348,830 cycles against the
table's median-of-nine 53,508,092.

Per-message figures come from an `rdtsc` pair around each handler; the harness
measures its own overhead (~40 cycles, comparable to the work itself) and
subtracts it. Throughput comes from a separate, completely uninstrumented
replay, so it is the honest end-to-end number.

| type | share | p50 | p99 | p99.9 |
|---|---|---|---|---|
| `A` add | 47.0% | 42 | 272 | 536 |
| `D` delete | 44.9% | 90 | 390 | 776 |
| `U` replace | 4.5% | 124 | 478 | 914 |
| `E` execute | 2.9% | 82 | 386 | 758 |
| **all** | | **66** | **354** | **706** |

**131 cycles/msg**, 62 ns/msg, 16.3M msg/s.

## What actually mattered

Weighted by share, the per-type p50s come to ~62 cycles/msg. Throughput said
**131**. Less than half the time was in the steady state.

The largest single sample was **53 million cycles** — on a one-million-message
replay, one message accounted for ~40% of total runtime. That was the pool
allocating its first slab: `1<<20` orders x 40 bytes = **42MB**, allocated and
page-faulted on the very first `A`.

Chunk size against cost, pinned, median of 5:

| first chunk | slab | cycles/msg | max sample |
|---|---|---|---|
| 2^20 | 41.9 MB | 132.2 | 53,470,476 |
| 2^18 | 10.5 MB | 87.5 | 13,360,790 |
| 2^16 | 2.6 MB | 81.8 | 3,471,238 |
| 2^14 | 0.7 MB | 80.6 | 1,085,922 |
| 2^12 | 0.2 MB | **63.3** | 485,252 |

Cost tracks slab size almost exactly, and p50 barely moves across the whole
sweep — this is pure first-touch cost, not steady-state work.

Smallest is not the answer either: tiny chunks mean many allocations and orders
scattered across many small blocks, which costs locality on a day with hundreds
of thousands of live orders. **Chunks now start at 4,096 orders and double to a
cap of 262,144**, so the first allocation is trivial while the chunk count stays
O(log n) and the bulk of orders still land in a few large contiguous blocks.

| | cycles/msg | p50 | p99 | p99.9 | max sample |
|---|---|---|---|---|---|
| baseline | 131.0 | 66 | 368 | 752 | 53,508,092 |
| geometric chunks | **79.6** | 58 | 366 | 728 | **551,020** |

**1.65x faster, 39% fewer cycles per message**, and the worst message improves
by 97x. On the PMU machine the same change measures **1.73x** (142.31 -> 82.04
cycles/msg), and throughput goes from 25.3M to 43.9M messages/second.

## The plan's five candidates, one by one

The build plan lists five likely wins. This is all of them, including the one
that was never tried — an accounting that quietly covers four of five reads as
a complete sweep, which it is not.

| # | candidate | outcome |
|---|---|---|
| 1 | open addressing instead of `unordered_map` | already in place from phase 3 |
| 2 | order struct fits a cache line | already in place: `Order` is 40 bytes, held by a `static_assert` |
| 3 | branch reordering, `A`/`D` first | **tried, measured −1.5%.** Reverted — see below |
| 4 | remove the `std::function`/virtual indirection | already in place from phase 3 (templated dispatch) |
| 5 | prefetch the next order in the free list | **never tried** |

Candidate 5 is untested, not rejected. The reason it was not pursued is the same
finding the two flat results below establish — on this workload the cost is
memory traffic on the *first, cold* access to an order, and the free-list head is
already hot — but that is a prediction, and this file exists precisely because
two other predictions of the same kind measured flat. It should be measured
before it is believed.

The win that did land was not on the plan's list at all: the pool's slab size.

### Two predicted wins that were not

The two candidates that were tried and measured flat:

**Hoisting `A` and `D` ahead of the dispatch switch** — they are 92% of the
feed, so testing them first should shorten the common path. Measured
**-1.5%**, i.e. slightly worse. The compiler already builds a jump table, and
the extra compares cost the other 8% of messages without helping the rest.
Reverted.

**Single-probe delete** — every execute, cancel, delete and replace looked the
order up in the ref map and then erased it by key, walking the same probe chain
twice. Eliminating the second walk is strictly less work, so it should show up
on the 45% of messages that are deletes. Measured **-0.5%: nothing**. The second
probe touches the exact cache lines the first one just pulled in, so it is
nearly free; a delete's cost is the first, cold access. Kept, because it is less
work and a cleaner API, but it is not a speedup and is not counted as one.

The lesson both share: on this workload the cost is memory traffic, not
instructions. Removing instructions from a path that is waiting on cache buys
nothing.

## Confirmed with hardware counters

The sweep above was run on a machine with no PMU, so the mechanism was inferred
rather than measured. It has since been confirmed on real hardware — Intel,
3.60GHz, WSL2 Ubuntu, `perf` with working hardware counters — using two builds
that differ only in the pool's chunk size:

```bash
g++ -std=c++20 -O3 -Iinclude tools/book_bench.cpp -lz -o bench_new
g++ -std=c++20 -O3 -DITCHBOOK_POOL_FIRST_CHUNK="(1u<<20)" \
                   -DITCHBOOK_POOL_MAX_CHUNK="(1u<<20)" \
    -Iinclude tools/book_bench.cpp -lz -o bench_old

perf stat -e page-faults,minor-faults,cache-misses,instructions,cycles \
    -- taskset -c 2 ./bench_old data/raw/bench.gz
```

| counter | 42MB slab | geometric | change |
|---|---|---|---|
| **page-faults** | 69,640 | 28,876 | **-58.5%** |
| **cache-misses** | 5,196,031 | 2,745,508 | **-47.2%** |
| **instructions** | 2,031,377,218 | 1,996,287,051 | **-1.7%** |
| cycles (whole process) | 1,437,389,398 | 1,245,317,483 | -13.4% |
| kernel time | 0.1207s | 0.0357s | **-70.4%** |
| cycles/msg (replay only) | 142.31 | 82.04 | **-42.4%** |

**The page-fault count identifies the cause exactly.** 40,764 faults disappear.
The benchmark constructs four books — one instrumented pass plus three timed
repeats — so that is 10,191 pages per book, or **41.7MB**. The slab no longer
being allocated is `1<<20 x 40` bytes = **41.9MB**, or 10,240 pages. The two
agree to **99.5%**: the eliminated faults *are* that slab, and nothing else.

**Instructions barely move (-1.7%), which is the point.** The optimisation
removes no work from the steady state. A change that made the book do less would
show up here; this one does not. What it removes is the kernel zeroing 42MB of
fresh pages, which is why kernel time falls 70% while user-space instruction
count stays put.

Cache misses falling 47% was more than expected. Cold, never-touched pages miss
on every first access, and zeroing 42MB evicts everything else on the way
through — so the same first-touch cost shows up in two counters at once.

### Where the prediction was imprecise

The prediction written before the run was "page-faults drop a lot, instructions
barely move, cycles down ~40%". The first two landed. The third needs a
correction: **process-level `cycles:u` fell only 13.4%**, not 40%, because
`perf stat` measures everything the process does — gzip-decoding a 30MB feed,
four book constructions, the instrumented pass — while the 42% figure is the
replay loop alone.

The two are consistent. Three timed replays at (142.31 - 82.04) cycles/msg is
181M cycles saved, against a process-level delta of 192M; the remainder is the
instrumented pass. Quoting the 42% against a whole-process counter would have
been wrong, and the counter is what caught it.
