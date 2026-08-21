# Phase 10 — how the latency will be measured, written before it is

This document exists before the code it describes. That ordering is the point:
every trap below is one that produces a *plausible* number, and a plausible
number written down first is much harder to argue with later than a caveat
appended to a result you like.

Phase 4's write-up opens with "the measurement mistake that came first" — a 12%
speedup from a change that did nothing, because the process was unpinned. That
mistake cost a week and was caught only because the same change measured
differently on a second run. The four sections here are the same class of
mistake, at a scale where nothing will catch them for you.

## 0. What is being measured, and what the current numbers are not

Every latency figure in this repository today is **handler cost**: cycles
between `cycles_begin()` and `cycles_end()` around a function call, in a single
thread, on a feed already in memory. 82 cycles per message is a true statement
about a function.

It is not latency. A trading system means, by latency, the time from *bytes
arriving* to *book updated* — across a thread boundary, with a queue in the
middle, under a load that may exceed what the consumer can absorb. There is no
thread boundary in this repository, so there is no such number, and phase 10
exists to create the boundary rather than to speed anything up.

The sample is `(book applied) − (packet arrived)`, and the two timestamps are
taken on **different threads pinned to different cores**. Section 2 is about why
that sentence is dangerous.

## 1. Coordinated omission

The trap: a load generator that waits for each response before sending the next
one, or that slows down when the system under test slows down, **stops sampling
exactly when the answer gets interesting**. If the book stalls for 10 ms, a
self-pacing sender sends nothing during the stall and records no slow samples —
so the percentile table describes the periods when nothing was wrong.

It is called coordinated omission because the load generator has quietly
coordinated with the system to omit its worst behaviour, and it is the single
most common way a latency graph lies.

The rule here:

* The sender sends on a **fixed schedule** computed in advance from the rate,
  not from anything the receiver does.
* It records the **intended** send time for every message alongside the actual
  one.
* Its own lateness — actual minus intended — is measured and reported as its own
  distribution, **separately** from wire-to-book. A sender that could not keep
  its schedule has invalidated the run, and the only way to know is to measure
  it.

If the sender's lateness is not small compared with the latency being reported,
the run is measuring the sender and the results say so. `mold_replay_udp` warns
above a **10 µs p99.9**, because loopback wire-to-book is measured in
microseconds and a sender tail of the same order contaminates it completely.
The first version of that check used a millisecond and would have passed a run
whose generator was two orders of magnitude noisier than the thing it measured.

### The sender is also the machine's qualification test

Two things had to be fixed before its own numbers were usable, and both are
worth knowing because they are the shape of mistake this section is about.

It decompressed inside its send loop, which put a single ~5.8 ms outlier at
every rate — the schedule started before gzip's first inflate, so packet zero
was always late by the cost of opening the stream — and gave it a tail that grew
with the rate, because a generator competing with the system under test for CPU
is not measuring it. Packets are preloaded now, the clock starts at the first
send, and the memory that costs is bounded and refused above a stated limit
rather than discovered as a swap storm.

What remains is not the program. Sweeping the spin margin on a shared container:

| spin margin | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| 100 µs | 267 ns | 96 µs | 259 µs |
| 500 µs | 52 ns | 27 µs | 84 µs |
| 2000 µs | 53 ns | 21 µs | 101 µs |

The margin buys an improvement and then stops, which says the residual is not
`nanosleep` overshoot but the sender thread being descheduled. **No margin fixes
that, and no amount of care in the code will.** A p50 of 52 ns and a p99.9 of
84 µs is a machine that cannot hold a schedule, and running the latency
experiment on it would produce a distribution whose tail belongs to the
scheduler.

So the first thing to run on any candidate machine is the sender alone, against
a sink that does nothing. If its p99.9 lateness will not come under 10 µs, the
machine is disqualified before the receiver is even written — which is a much
cheaper way to find out than reading it out of a rate–latency curve later.

## 2. Clock domain — and the assumption this document exists to kill

The plan of record for two revisions said: *"arrival timestamps come from the
receiver's TSC, so wire-to-book is one clock, no cross-machine sync problem."*

That is wrong in a way that would not show up as an error. **The TSC is
per-core.** Phase 10 subtracts a timestamp taken on the receiver's core from one
taken on the book thread's core, and that is the first time this repository has
ever done so. `tsc_is_invariant()` (`bench/rdtsc.hpp`) checks `constant_tsc` and
`nonstop_tsc` — that the counter ticks at a fixed rate regardless of frequency
and C-state. It says **nothing** about whether two cores agree on what time it
is. A constant offset between them lands directly in every reported latency,
and if it is negative the histogram fills with impossible values that look like
a clock bug rather than what they are.

So, before any latency table is produced:

1. **Measure the offset.** `tools/tsc_offset.cpp`. Two pinned threads ping-pong
   a token; A stamps `t1`, B stamps `t2`, A stamps `t3`. Whatever instant B's
   stamp really corresponds to lies in `[t1, t3]` on A's clock, so
   `offset ≈ t2 − (t1+t3)/2` with `uncertainty ≤ (t3−t1)/2`. The tightest sample
   is the fastest one, not the average of samples that were interrupted.
2. **Report the bound, not the estimate.** An estimate smaller than the method's
   own resolution has not been distinguished from zero, and printing it as an
   offset is printing the noise floor as a finding. The tool's verdict is
   therefore a three-way one: unresolvable (report the bound), a protocol
   artefact (the sign fails to reverse when the roles do), or a real offset.
3. **If it is real and not negligible**, do not correct for it — use
   `CLOCK_MONOTONIC_RAW` for the cross-thread sample and keep the TSC for
   intra-thread work only. A correction is a model; a different clock is a fact.
4. **State which clock produced which table.** Every one of them.

The tool also reports what it could not control: which timestamp source the
build actually compiled in — on a platform without `rdtsc`, `bench/rdtsc.hpp`
falls back to `clock_gettime`, which is system-wide and the question does not
arise — and whether thread pinning was available at all, because on a machine
that cannot pin there is no guarantee the two threads ever ran on different
cores and a small answer means nothing.

### What it found about itself, first

The first version reported **−37.5 ns forward and −38.9 ns reversed**. Same
sign, same magnitude — and a real offset reverses when the roles do. The sign
check, built to separate clock offsets from protocol artefacts, caught a
protocol artefact on its first run.

The cause was mixing stamping instructions: `t1` used `cycles_begin()`
(`lfence; rdtsc`) while `t2` and `t3` used `cycles_end()` (`rdtscp; lfence`).
`rdtscp` waits for prior instructions to retire and `rdtsc` does not, so `t1`
sat systematically earlier within its own instruction stream than `t3` did
within its, dragging the midpoint later and making `t2` look early by a fixed
amount in both directions at once. All three stamps now use the same
instruction.

That halved it and did not remove it, which is the more useful outcome: what
remains is inside the method's resolution, so the honest report is a bound. On
this hardware, two pinned threads at 100,000 samples give a fastest round trip
of ~262 ns and an offset estimate of ~40 ns — **under the ~131 ns the method can
resolve, therefore not distinguishable from zero**.

For phase 10 that is a usable answer rather than a clean one. Loopback
wire-to-book will be measured in microseconds; a bound of ~131 ns is a small
fraction of that and the tables can subtract the two clocks — provided they
**quote the bound beside the number**, which is what standing rule 3 has always
required of a caveat.

### And the platform this cannot be measured on

Run on an Apple-silicon Mac, the same tool reported a fastest round trip of
**0 ns**, a median of 0 ns, and — in its first version — concluded that the
offset was "bounded under 0 ns". That is the most reassuring sentence it could
possibly have produced and it was worth nothing: a round trip of zero is not a
fast handoff, it is a clock that cannot tell the two ends apart.

The tool now measures **its own clock's granularity** before trusting any
interval, by sampling back to back and keeping the smallest non-zero gap, and
refuses to bound anything when the round trip is not several ticks wide. That
check turned a confident number into `VERDICT: UNMEASURED`, which is the correct
output.

Two consequences, and both constrain where phase 10's numbers can come from:

* `bench/rdtsc.hpp`'s fallback now uses `CLOCK_UPTIME_RAW` on Apple rather than
  `CLOCK_MONOTONIC` — Apple's un-adjusted counter, finer and cheaper, since it
  skips the NTP-adjusted path. It is still system-wide, which is this
  fallback's one advantage over the TSC: there is no per-core offset to find.
* **The latency runs need Linux.** macOS exposes no call that binds a thread to
  a core, so the tool reports pinning as unavailable and says a small offset
  there is not evidence of anything. Section 4 requires pinning, and a headline
  latency number from an unpinned machine would be the 19.3%-variance mistake
  from phase 4 repeated at a larger scale. The book and the sweeps can run
  anywhere; the wire-to-book histogram cannot.

## 3. Loopback is not a network

The numbers this phase produces come from UDP over loopback. That means:

* **No NIC.** No driver, no DMA, no interrupt coalescing, no ring buffer on a
  card. A real feed handler's first few microseconds live there and none of it
  is here.
* **No kernel bypass.** No DPDK, no `AF_XDP`, no Onload. Deliberately out of
  scope: the point is a defensible ring and an honest measurement across it, not
  a number that competes with a product.
* **No switch, no cable, no other host.** Loopback copies within one machine's
  memory, so the transport is faster and *far* more predictable than any wire.

Therefore the headline is **"wire"-to-book on loopback**, with the quotation
marks meant. What would change on real hardware belongs in the results document,
stated as unknowns rather than estimated.

### 3a. The drops that are not yours

On loopback, the kernel's socket receive buffer overflows **before** the ring
does. If the only counter is ring-full events, a run can lose thousands of
packets upstream and report a clean sheet — which is the exact failure mode
phase 7 exists to prevent, reintroduced one layer higher.

So:

* `SO_RCVBUF` is raised **and the achieved value read back with `getsockopt`**,
  because Linux silently caps it and the value you asked for is not evidence.
* Kernel drops are read from `/proc/net/udp` (and the `recvmmsg` error counters)
  and reported **separately** from ring drops, at every rate, in every table.
* A rate is only "sustainable" if **both** are zero.

## 4. Pinning, interleaving, and what counts as a result

Phase 4's rules, unchanged, because they were not about phase 4:

* **Pin.** Three actors — sender process, receiver thread, book thread — to
  three cores, `taskset` documented in the results. Unpinned, `book_bench`
  varied 19.3% between identical invocations; that variance is enough to
  manufacture any conclusion. Where the platform has no `taskset` (macOS), the
  results say so rather than implying a tightness they do not have.
* **Interleave.** A, B, A, B — never all of A then all of B — so that machine
  drift cannot be attributed to the change under test.
* **Best of N, with the median beside it.** Noise here is one-sided: another
  process can make a run slower and nothing can make it faster.
* **Anything inside the noise floor is not a result** and gets reported as flat.
  Three predictions in `bench/README.md` measured flat and are still there.

## 5. What the deliverable is

Not five percentiles. `Histogram` grew logarithmic buckets and a CSV renderer in
phase 8 for a reason: two builds can report the same p50 and p99 and have
completely different shapes — one tight cluster with stragglers, or two modes
because a branch is taken half the time. The second is a mechanism you can go
and find; a percentile table hides it.

So every latency claim ships as a **distribution**, with percentiles quoted from
it, and the rate–latency curve ships with the knee and the cliff annotated and
the drop counts — both kinds — beside every point.
