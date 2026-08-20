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
by 97x.

## Two predicted wins that were not

The build plan lists likely optimisations. Two were already in place from phase
3 (open addressing instead of `unordered_map`; templated dispatch with no
virtual call). Two more were tried and measured flat:

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

## What is missing, and why

The build plan's done-condition asks for a hardware counter behind every
speedup. **This machine cannot provide one.** `perf` is not installed and cannot
be, and the PMU is not virtualised — `perf_event_open` returns `ENOENT` for
`instructions`, `cycles`, `cache-misses` and `branch-misses` alike.

So the mechanism above is established by a controlled sweep rather than by a
counter: cost tracks slab size across five orders of magnitude while p50 stays
flat, which isolates first-touch page-fault cost about as well as an experiment
can without a PMU. On a machine with `perf`, confirm it directly:

```bash
perf stat -e page-faults,cache-misses,instructions,cycles \
    ./build/book_bench data/raw/bench.gz
```

`page-faults` and the `max sample` column should move together; `instructions`
should barely change, since this optimisation removes no work from the steady
state.
