# Phase 12.8 — Tick-to-trade, decomposed. Design of record.

Written before the instrumentation exists, reviewed adversarially before it was
written down, and corrected by three findings that would each have produced a
published number that was wrong. Every figure below is measured on this machine
or read from an artifact in `validation/`; none is estimated.

The measurement **needed bare metal and has now been taken**: three boots on
2026-08-28, recorded in §11. What follows is the design and the corrections that
preceded them, kept in the order they were found rather than rewritten to look
like it went smoothly. The results are in
[`phase12-8-results.md`](phase12-8-results.md), generated from the artifacts.

---

## 0. Five things the plan says that do not survive contact with the code

`docs/build-plan-9-12.md` §12.8 is the input, not the authority.

**"t₀ → t₁ book updated (phase 10's number)."** It is not phase 10's number and
must never be printed as continuous with it. Phase 10's figure comes from
`tools/wire_to_book.cpp`, which is `receiver thread --ring--> book thread`: two
pinned threads, a 65,536-slot SPSC ring, and *one* arrival stamp amortised
across a `recvmmsg` batch. `tools/strategy.cpp` has **zero** `std::thread`, no
ring, and one `::recvfrom` per datagram with `seq.on_packet()` and `bk::apply()`
inline in the same `poll()` loop. Different program, different queue, different
stamp granularity. 12.8 measures t₀→t₁ afresh and prints phase 10's figure only
in a separate column labelled with the harness that produced it.

**"t₄ → t₅ match + ITCH publish."** Its dominant term is not latency. A strategy
order is filled only when a *historical* order at the same price, added after
it, is executed — `tools/strategy.cpp`'s own banner says so. So t₄→t₅ is
dominated by how long the quote sat in the book waiting for MSFT's own flow, i.e.
market structure divided by `--multiplier`. At 1× that is up to tens of seconds,
which also saturates `Histogram`'s `UINT32_MAX` clamp (1.193 s at the measured
3.599 cycles/ns). It is excluded from every nanosecond total and every stacked
bar, and reported separately in **replay seconds**.

**"the full round trip."** There is no round trip for most orders. ITCH names
resting orders only. On the recorded day (`validation/closed-loop-2019-12-30.json`)
1,180 of 1,200 orders were ever seen on the tape; the rest crossed and can have
no tape record at all. The round trip is defined on a selected subpopulation and
is published with the census that names the exclusions.

**"the hops sum to the total."** If the hops are differences of stored stamps,
Σ(hops) = t_last − t_first **by telescoping, unconditionally**. It holds with a
10 µs cross-process clock offset, with a stamp in the wrong function, and with an
entirely un-instrumented region between two stamps — the time is still in *some*
hop, merely labelled wrong. As a check it cannot fail, which is this repository's
signature defect: `scripts/closed-loop-check.sh` says *"this repo has produced a
vacuous gate four times, and three of those were a detector that could not run
being scored as a detector that found nothing."* §5 replaces it.

**"Stacked bar at p50 and p99.9."** Percentiles are not additive.
p50(a)+p50(b) ≠ p50(a+b), and Σ p99.9(hop) overshoots p99.9(total) because the
chance of every hop spiking on one chain is the product of their individual
chances. A stack of marginal percentiles depicts a round trip that never
happened. §5.3 gives two constructions that do sum.

---

## 1. Three findings that would each have produced a wrong number

These came from an adversarial review of the design, before code existed. Each
is verified against the source, not argued from principle.

### 1.1 The headline was anchored to a message with no causal role

`tools/strategy.cpp:470-500`: the quote block sits *after* the drain loop, and
reads the book **at quote time**:

```cpp
const bk::Book* b = belief.set.peek(belief.locate);
if (b != nullptr && b->best_bid(&bid)) base = bid;
```

The decision's input is therefore the *post-drain* book — the last message
applied in the iteration. The message that crossed the counter
(`belief.applied - last_quote_at >= opt.quote_every`) only crossed a counter; its
content never reaches the price. Worse, `last_quote_at = belief.applied;` is
assigned *at quote time*, post-drain, so the sampling stride is not
`quote_every` but `max(quote_every, messages delivered in the wakeup that
crossed it)` — data-dependent, and it inflates under load.

A t₀ anchored on the trigger message therefore times *"the arrival of some
datagram a few hundred messages before the decision, through an unbounded amount
of unrelated work, to the write"* and calls it tick-to-trade. A reader
benchmarking that against a venue's published figure, or 12.9 calibrating a
latency model from it, would be misled.

**Resolution.** The headline reaction-path number is **t₁′→t₃** — post-drain
decision to write. **t₀→t₃ is reported separately and explicitly as
"arrival-to-write including queue drain"**, with the drain term named and the
realised stride recorded per sample and published as a distribution rather than
assumed.

### 1.2 p99.9 at the achievable sample size is mostly scheduler noise

Chain A yields roughly one sample per order — order 1,200 per run, ~12,000 over
ten runs. The p99.9 of n = 12,000 is the twelfth largest sample. From
`validation/cpu-jitter-baremetal.json`, cpu 7: 602 gaps over 10 µs in 20 s =
**30.1 gaps/core/s**, recorded-gap p50 **15,092 ns**, p99.9 **24,732 ns**, max
**43,298 ns** — and that is the *bare-metal* box. A chain spanning a full drain
occupies the core for tens of microseconds, so the expected number of chains hit
by a >10 µs gap is order 1–7 out of the twelve that *define* p99.9.

So between roughly 6% and 60% of the samples setting the published p99.9 are
descheduling events wearing a hop's label, and the contaminated fraction is a
property of the machine and the chain length, **not of n** — more runs estimate
the same contaminated quantity better.

**Resolution.** The headline tail is **p99**. p99.9 is published only with a
**gap-overlap census** beside it, and two values are printed — all chains and
gap-free chains — with the tagged count. A p99.9 without that census is not
printed at all.

### 1.2.1 The gap-overlap census, built — and there is no watchdog

The design above asked for a co-pinned watchdog. There is no good version of
that: pinned to the **same** core it contends with the thread it exists to
observe, destroying what it measures; pinned to a **different** core it observes
the wrong core, because gaps are per-core.

The kernel answers directly instead. Over any window,

    off_cpu = wall_elapsed − thread_cpu_elapsed

is the time the thread was not running, attributed exactly to that window, with
no second thread and no contention — the same quantity `cpu_jitter` already
reports in aggregate. `CLOCK_THREAD_CPUTIME_ID` was measured before it was
trusted:

| | |
|---|---|
| observed granularity | **131 ns** (`clock_getres` claims 1 ns) |
| across a 5 ms sleep | wall 5,392,950 ns, CPU 42,708 ns → **off_cpu 5,350,242 ns** |
| over 2,000 × ~11 µs of pure work | off_cpu p50 **0 ns**, p99 130 ns, 1 of 2,000 over 5 µs |
| cost | 522 cycles, against 68 for `CLOCK_MONOTONIC` |

It sees a deschedule exactly and almost never reports one that did not happen.
The threshold is **5 µs**: far above the instrument's 130 ns noise floor and far
below the 15,092 ns median gap it exists to catch.

**The clock is read outside the interval**, immediately before t₁′ and
immediately after t₃, so its ~145 ns cost does not inflate a headline hop that
may be single-digit microseconds on bare metal. The consequence is that the CPU
window strictly contains the wall window, so a chain that never stopped reads
slightly **below** zero — a typical −626 ns here. That is the bracket's own zero,
it is reported as such, and it biases toward **missing** a gap rather than
inventing one.

**The census checks itself every run**, because one that tags nothing is
indistinguishable from one that cannot tag. Each report states whether the
*slowest* chains carried off-CPU time.

### 1.2.2 What the census disproved, in this document

An earlier commit message for this phase said of a 17.7 µs `t2→t3`: *"That is
the scheduler, on a box cpu_jitter says does not hold a CPU."* **The census says
otherwise and the census is right.** Measured on the release build, cpus 13/14:

| hop | Debug + ASan | Release |
|---|---:|---:|
| `t1′→t2` decision | 1,224 ns | **90 ns** |
| `t2→t3` encode, frame, write | 17,702 ns | **8,733 ns** |
| headline `t1′→t3` | 19,214 ns | **8,845 ns** |

Roughly half of it was **AddressSanitizer**, and the remaining 8.7 µs is genuine
CPU time — off_cpu is ~0 for every chain, including the twelve slowest, which
ran 27–40 µs against a median of 8.8 µs and were **running the whole time**.
That tail is the loopback `write()` syscall path, not descheduling. So the hop
is also misnamed: the encode is the 90 ns in `t1′→t2`, and `t2→t3` is
overwhelmingly the write.

Two things follow for the boot. Measurements must be taken on the **release**
build, with the sanitised build used only for the correctness gates; and the
per-hop names in the results document must say `write` where they currently say
`encode, frame, write`.

### 1.3 Every chain-A sample is taken at the loop's most favourable instant

The UDP drain runs to `EAGAIN`; the quote block executes after it, at most once
per `poll()` iteration. So t₁′→t₂→t₃ is *always* measured with an empty receive
queue, the book's lines just touched by the drain, and a TCP socket idle for the
whole drain. Jittering the quote cadence does not help: the bias is in **loop
phase**, not stride, and every stride lands at the same phase.

**Resolution.** Stated in the results document as a condition on the numbers —
all chain-A hops after t₁′ are conditioned on an empty receive queue — with the
drain-burst length distribution published beside them. A separately labelled
`--quote-in-handler` arm, which quotes from inside `FeedHandler::on_message` and
so samples at a random loop phase, is run as a comparison. It changes the program
under test, so it is a variant and never the default, and the four-arm gate is
re-run against it.

---

## 2. What is measured: two chains and one interval

The plan's seven hops are one causal chain only if t₄→t₅ is a latency. It is
not, so the chain is rebuilt as two chains joined by an interval reported in
different units.

**Chain A — tick-to-trade.** One sample per order, all stamps in the strategy
except the last two.

| stamp | where |
|---|---|
| t₀ | strategy, after `::recvfrom` returns, per datagram |
| t₁ | strategy, after `bk::apply` for the message that crosses the quote counter |
| t₁′ | strategy, first line of the quote block, after the whole drain |
| t₂ | strategy, after the price is computed and `px > 0` passes |
| t₃ | strategy, at the `::write` whose cumulative bytes first cover this order's frame |
| t₃′ | exchange, at the `::read` that completes this order's frame |
| t₄ | exchange, in `send_accepted` / `send_reject`, with the response type recorded |

**Headline: t₁′→t₃** (§1.1). Reported beside it: t₀→t₃ including drain.

**The resting interval**, t₄ → t_A, in replay seconds with `--multiplier`
printed beside it. Never stacked, never in a cycle histogram.

**Chain B — fill report.** One sample per maker fill, anchored at the aggressor
rather than the order, so every fill is an independent traversal.

| stamp | where |
|---|---|
| t_A | exchange, before `replayer.apply` for an `'E'` |
| t₅ₐ | exchange, in `aggress()` after `apply_external_fill` returns, before `emit_exec` |
| t₅_b | exchange, in `udp_send`, at the `::sendto` carrying that `'E'` |
| t₆′ | strategy, `::recvfrom` return for that datagram |
| t₆ | strategy, in `FeedHandler::on_message` when the `'E'` is seen — in **both** the owned and the parked branch |

**t₅ₐ→t₅_b is its own hop, the packing delay**, because `mold::Publisher::add`
flushes only when the datagram fills or at the 250 ms heartbeat. The recorded day
published 1,222,930 messages in 27,218 data packets — **44.9 messages per
datagram**. Folded into "match + ITCH publish" this is a hop nobody can explain
whose value is a function of `--mtu`; split out and named, it has a knob, and
halving `--mtu` should roughly halve it. That sweep is run.

---

## 3. The time base

**`clock_gettime(CLOCK_MONOTONIC)` for every stamp in both processes**, plus one
`rdtscp` read with its `aux` register captured beside t₀ and t₃ in the strategy
as a migration witness and a second instrument.

The cost objection does not survive measurement. On this box, net of the
instrument's own 28-cycle zero, at 3.599 GHz:

| | p50 (cycles) | net | ns |
|---|---:|---:|---:|
| empty region (the instrument's zero) | 28 | — | — |
| `clock_gettime(CLOCK_MONOTONIC)` | 68 | **40** | 11.1 |
| `rdtscp` + `lfence` (`cycles_end`) | 68 | **40** | 11.1 |
| `lfence` + `rdtsc` (`cycles_begin`) | 58 | 30 | 8.3 |
| `mono_ns()` as it will be written | 72 | 44 | 12.2 |

They are a wash, so the decision is made on three other grounds. *One epoch, one
scale*: `calibrate_cycles_per_ns()` is run independently in each process and two
calibrations of the same counter differ by ~1e-4 relative, so nanoseconds would
not sum across the process boundary even with perfectly synchronised counters.
*The stamps join the machinery that causes the outliers*: `session.tick()`, the
heartbeats, `kTickPeriodNs` and `--ack-delay-ms` all already run on
`CLOCK_MONOTONIC`. *Several stamps already exist* as `wall_now_ns()`.

**What this does not buy, stated narrowly.** On x86 with `clocksource=tsc` the
vDSO computes `CLOCK_MONOTONIC` as a global mult/shift/offset over a raw
`rdtsc`, with no per-CPU correction. Cross-core skew passes straight through.
Claiming immunity would repeat the error `tools/tsc_offset.cpp` exists to
correct: *"It is not one clock. The TSC is per-core."*

**Exactly two hops span processes**: t₃→t₃′ and t₅_b→t₆′. With δ = (exchange
clock − strategy clock), one measures true+δ and the other true−δ, so their
**sum is δ-free and only their split is exposed**. The headline t₁′→t₃ and every
intra-process hop are clock-exact. The error bar goes on two segments of the
figure, not on the whole table.

On this machine `tsc_offset` reports the offset **not distinguishable from zero,
bounded under 52 ns** (fastest round trip 372 cycles, uncertainty 186 cycles =
51.7 ns, estimates −17.2 / −19.4 ns in the two directions). The bar for 12.8 is
**relative, not the fixed `BOUND_NS = 1000.0` of `scripts/pinned-run.sh`** — that
threshold was set against a ~5.3 µs wire-to-book p50 and 12.8's hops are an order
of magnitude smaller by construction. The bound must not exceed **10% of the
smallest reported cross-process hop's p50**, and both numbers and their ratio go
in the artifact.

**Negative hops are never clamped.** `tools/wire_to_book.cpp:852` does
`now > s.arrival ? now - s.arrival : 0`, which is safe within one thread and
catastrophic across cores: it substitutes 0 with no counter, biases p50 down, and
destroys the only live evidence that the clock claim is wrong. Intra-process
negatives **abort the run** — they are a harness bug, not a clock finding.
Cross-process negatives are counted, their worst printed beside every percentile,
and any larger than the `tsc_offset` bound **vetoes** the run.

---

## 4. What enters the histograms

- **t₀→t₁, all messages** — every applied message, with `msg_index_in_datagram`
  recorded beside it, because at 44.9 messages per datagram the fortieth
  message's arrival-to-applied genuinely includes the parse and apply of the
  thirty-nine ahead of it. Without the index, that spread reads as a book or
  cache effect and is neither.
- **t₀→t₁, trigger messages only** — published as a second column. If the two
  disagree the headline is reported against the trigger column and the
  disagreement is stated, not averaged.
- **Chain A** — every order reaching t₃, **including rejects**, with the response
  type recorded. `send_reject` returns before the book walk, so rejects are the
  cheap path; keeping only accepts would delete the fast mode and inflate
  t₃′→t₄'s p50.
- **Chain B** — every maker fill of a strategy reference, including parked ones.
- **Excluded** — chains whose paired `rdtscp` stamps disagree on CPU (pinning
  silently failed), chains whose arena slot overflowed, and anything with a
  negative intra-process hop, which aborts instead.

**The census must balance to `orders_sent`.** Every order is classified at t₃:
accepted / rejected / crossed-and-never-named-on-the-tape / rested-and-unfilled
at exit / in flight when the run ended. The run is refused if the classes do not
sum. The counter that does not exist today — *distinct orders with at least one
maker fill* — is added; today only bounds are available.

**Completion is not random and it selects toward the tail.** An order reaches
chain B only if it rested *and* a historical order at its price traded after it,
which selects for busy stretches of tape — exactly where chain A is worst. So the
headline is reported over **all** orders with the filled-subset column printed
beside it, so the selection effect is visible rather than inferred.

---

## 5. The sum rule, restated so it can fail

### 5.1 What the identity is

Σ(hops) = t_last − t_first is arithmetic, not a check (§0). It is written down
for one reason: it localises δ. The gate line never reads `hops sum to total:
PASS`.

### 5.2 Four checks that can actually fail

**(a) Instrumented coverage.** A stamp at the top and bottom of the strategy's
`while (true)`, and per iteration the sum of every hop stamped inside it.
`iteration_wall − Σ(hops)` is un-instrumented time, published as a coverage
fraction with p50 and p99. This catches an entire region nobody is timing — the
failure the telescoping identity structurally cannot see. A decomposition whose
coverage is 60% is not a decomposition and the report says so.

**(b) Two instruments on one number.** t₁′→t₃ measured by the `CLOCK_MONOTONIC`
chain and by the adjacent `rdtscp` pair, converted with the recorded
`cycles_per_ns`. They must agree within the measured stamp cost at p99.

**(c) Sign gates.** §3.

**(d) Count and set identities**, machine-independent and runnable today:
`orders_sent` == chain-A records reaching t₃ == exchange `ouch_accepted +
ouch_rejected`; `maker_fills_from_feed` == chain-B records reaching t₆; every
chain-B record joins to exactly one chain-A record; the census balances.

**Why counts and not booleans.** `tools/wire_to_book.cpp` closed the socket
before reading `/proc/net/udp`, so `kernel_drops` returned 0 on every run ever
made and its exit code was unreachable — a gate whose input was a constant. A
boolean `instrumented: true` is the same defect. Every hop carries a
`stamps_taken` count that must be non-zero and must equal the expected message
count.

### 5.3 The two constructions that do sum

**Per-sample decomposition.** Rank completed chains by their own total; stack the
hops of the chain at the p50 rank and the chain at the p99 rank. Each sums to its
bar's height by construction, and it answers the question a reader actually has:
when the round trip was slow, where did the time go?

**Tail-conditional mean.** For C = {chains whose total ≥ q₀.₉₉(total)}, plot
E[hop | C], which sums exactly to E[total | C] by linearity. Labelled *"mean
decomposition of the slowest 1% of chains"*, never *"p99 per hop"*.

Alongside, in a table **explicitly marked non-additive**: per-hop p50/p99 with n
and order-statistic confidence intervals; the named residual
`Σ p99(hop) − p99(total)`; and the argmax-hop frequency within C — *"in N% of the
slowest chains the dominant hop was the packing delay"* — which is what
"explain the hop" means operationally.

---

## 6. What the harness refuses to do

Exit codes follow `scripts/pinned-run.sh`: **0** ran and quotable; **3** ran
correctly, not quotable; **1** broken; **2** usage; **4** precondition unmet.
*The artifact is always written*, with the verdict and every input it was
computed from inside the same object, because *"deleting the evidence is not the
same as declining to quote it."*

**Hard refusal (exit 1):** any negative intra-process hop; any arena
`reserve_exceeded`; `dropped_samples != 0`; any hop with `stamps_taken == 0`;
sequencer gaps or messages lost; pinning requested and not granted; a
strategy-vs-exchange share disagreement; a chain-B record joining to zero or to
more than one chain-A record; any `Histogram` saturation.

**Not quotable (exit 3, artifact written):** `cpu_jitter` on the exact pinned
cores shows any gap over 100 µs; the `tsc_offset` bound exceeds 10% of the
smallest cross-process hop p50; `current_clocksource` is not `tsc` at start, at
end, or differs between them; the two CPUs are SMT siblings or on different
physical packages; a cross-process negative larger than the offset bound; n below
the stated floor for a requested percentile; the two instruments disagree; the
git tree is dirty; coverage below its floor.

**Two vetoes, not warnings.** Two poll loops on **SMT siblings** share one core's
execution ports *and one TSC*, so `tsc_offset` reports ~zero and the run looks
well controlled while every hop is inflated by contention — a check that cannot
fail. **Different physical packages** is also a veto: cross-socket TSC
synchronisation is a firmware property and a box can keep `clocksource=tsc` while
the packages differ by far more than the bound. Pinning both processes to *one*
CPU is refused outright: t₃→t₃′ would then measure a context switch and be
publishable as a gateway latency.

**Three-state probes, always.** Every environment probe returns value / absent /
unreadable, and **absent is never a pass**. There is no `cpufreq` under WSL2 at
all, so a check written as "governor != powersave" passes silently on the machine
it exists to flag — the same shape as the kernel-drop gate.

---

## 7. Why this machine cannot produce the numbers, measured

**Every clock gate this repository owns passes on this WSL2 box.**
`/proc/cpuinfo` carries `constant_tsc`, `nonstop_tsc` and `tsc_reliable`;
`current_clocksource` reads `tsc`; `tsc_offset` reports pinning granted and the
offset bounded under 52 ns — clearing the existing `BOUND_NS = 1000.0` by a
factor of nineteen. A full table with p50 and p99.9 would be produced with no
gate having fired, and it would be wrong.

The signal that discriminates is `cpu_jitter`, and it is unambiguous:

| | this box (WSL2) | `validation/cpu-jitter-baremetal.json` |
|---|---:|---:|
| gaps > 10 µs / cpu / s | **~3,000** | **30.9** |
| worst gap | **2,571,878 ns** | **43,298 ns** |
| worst recorded-gap p99.9 | — | 24,732 ns |
| verdict | **does NOT hold a CPU** | `holds_a_cpu: false`, 0 gaps > 100 µs |

`cpu_jitter` on every core the run will use, from one barrier, with a
zero-gaps-over-100 µs bar, is therefore the **decisive** gate; the clock checks
are demoted to necessary-but-not-sufficient, and the artifact records why each is
not decisive.

**Validated here (machine-independent):** every join and arena; per-sample
monotonicity; the count and set identities of §5.2(d); zero allocations between
t₀ and t₃ and between t₆′ and t₆, via a counting `operator new` armed around the
regions; saturation detection; the census balancing; coverage plumbing; the
four-arm `closed-loop-check.sh` re-run against the **instrumented** binary, since
a compile-time switch would mean the gate runs binary A while the numbers come
from binary B. Plus two 12.8-specific mutation arms: move t₆ into
`Belief::retire()` and require arm 4 to catch it; stamp t₃ at
`client.send_unsequenced` instead of at the `::write` and require the coverage
check to catch it.

**Waits for the boot:** every absolute nanosecond figure, every percentile, and
**the ranking of the hops by size** — descheduling is not uniform across hops, so
on a box whose median recorded gap is 15 µs the *order of the bars* is itself an
artefact.

---

## 8. Already landed, ahead of the boot

The review found four allocations and a memmove **inside regions 12.8 will be
timing**. They are not measurement artefacts — they are in the path a real order
takes, on every order — so they were fixed first, measured, and gated.

| | before (p50 cycles) | after | net change |
|---|---:|---:|---:|
| SoupBinTCP frame send | 72 | 50 | **44 → 22 net cycles** |
| ...at p99.9 | 654 | 424 | |
| order token | 156 | 82 | **128 → 54 net cycles** |
| ...at p99.9 | 1,066 | 236 | **4.5× better tail** |

`send_unsequenced` (every order out) and `send_sequenced` (every Accepted and
Executed back) both did `std::vector<uint8_t> frame(len + 3);` — a malloc, a
zero-fill of bytes about to be overwritten, and a free, per message, in both
directions. That is the same value-initialisation mistake phase 10 found in
`Pool::grow`, where zeroing a chunk about to be overwritten cost 47% of a 5 ms
stall. The outbound drain did `erase(begin(), begin() + w)`, an O(remaining)
memmove per write, now a consumed-offset cursor. The token used `snprintf` with a
format string and a locale walk to produce fourteen fixed bytes; the replacement
is byte-identical over the tested range, which the four-arm gate also checks for
free because the token is what the exchange echoes back.

25/25 in ctest and all four arms green at 39 checks after the change.

Still to do before measuring: `my_refs`, `parked` and `Parked::execs` are
`unordered_*` containers inside t₆′→t₆ and become direct-indexed arrays keyed on
`ref & ~kStrategyRefBit`, which `RefSource` makes dense. They carry 12.7's
correctness logic, so they change with the instrumentation pass and the gate
grades one change at a time.

---

## 9. The bare-metal checklist

**Before the boot.** Build every binary **static** — the live session has no
compiler and may have no network: `g++ -O2 -std=c++20 -static -I include -o
build-pinned/<tool> tools/<tool>.cpp -lz -lpthread` for `exchange`, `strategy`,
`tsc_offset`, `cpu_jitter`. Put `data/sliced/MSFT.gz` (12 MB, licensed, not in
the repo) and a save script on the stick — a live session loses everything at
reboot. Record the commit SHA and verify the tree is clean.

**On the machine, before any measurement.**

1. **Re-read the topology on the day**; do not inherit phase 10's numbering.
   Enumerate `/sys/devices/system/cpu/cpu*/topology/{core_id,thread_siblings_list,physical_package_id}`
   and pick two CPUs on **distinct physical cores, same physical package**. Note
   that `nproc` reports the calling shell's affinity mask, not the machine.
2. Record `current_clocksource`, `scaling_governor`, `intel_pstate/no_turbo`,
   `/sys/devices/system/cpu/isolated`, `/proc/cmdline`, `/proc/version`, the
   hypervisor flag, `net.core.rmem_max`. Three-state each.
3. `cpu_jitter --cpus <A>,<B> --seconds 20` on exactly the chosen CPUs.
   **Refuse to proceed** if either shows a gap over 100 µs.
4. `tsc_offset --cpu-a <exchange> --cpu-b <strategy>`, both directions, JSON out.
   **Check the exit code before reading the file** — `pinned-run.sh` carries that
   guard twice because it happened.
5. **Do not use `--rt-priority` or SCHED_FIFO.** Phase 10 established that RT
   priority was itself the source of a 47 ms lateness spike via RT bandwidth
   throttling; dropping it took the sender p99 from 26,603,394 ns to 116,005 ns.
6. **Dry run first**: a short `--limit` slice at `--locate 5291`. Confirm
   **non-zero samples in every hop** and a non-zero maker-fill count before
   spending the boot. A hop with zero samples scored as a pass is the specific
   failure that already happened twice in 12.7.

**The measurement.** Ten repeats of the natural configuration (`--multiplier 1000
--quote-every 200 --max-orders 1200 --quote-shares 100 --quote-offset-ticks 2
--heartbeat-ms 250 --mtu 1400 --locate 5291 --symbol MSFT --wait-for-client`),
both processes pinned, ~62 s each. Then `--multiplier 500` and `2000` for the
resting-time classification; `--mtu 700` for the packing-delay mechanism; one
`--ack-delay-ms 250` run as the t₆ placement positive test. Roughly 25–30 minutes
of runtime; budget 90 minutes on the machine.

**After.** Re-run `cpu_jitter` and `tsc_offset` on the same pair and re-read
`current_clocksource`. Any change fails the run. Copy every artifact off the
stick before rebooting.

---

## 10. Status

**Built and exercised.** The design of record above; the hot-path fixes of §8;
`bench/rdtsc.hpp`'s `mono_ns()` and `cycles_end_cpu()`; `bench/histogram.hpp`'s
saturation counter; `bench/trace.hpp` (arenas that drop and count rather than
wrap); `bench/topology.hpp` (three-state probes, SMT and cross-package vetoes);
`Publisher::sequence_of_next_add()`; `SplitReplayer::set_fill_trace()`; chain A
and chain B stamped in both processes with `--cpu` and `--trace-out`;
`scripts/phase12-8-report.py` (the offline join and the refusals);
`scripts/tick-to-trade.sh` (the run harness and its pre-flight);
`scripts/tick-to-trade-selftest.sh` and `scripts/tick-to-trade-mtu-sweep.sh`.

**What the harness has been shown to do**, on this machine, where none of the
numbers may be published:

| | |
|---|---|
| chain A join | 800 of 800, no orphans, no duplicate tokens |
| chain B join | 33 of 33 on (reference, fill ordinal), every datagram found |
| two instruments on t₀→t₃ | 3.600 cycles/ns implied, against `tsc_offset`'s 3.599 |
| coverage of the completing iteration | p50 99.2% |
| MTU sweep, packing delay p50 | 144.5 µs → 117.1 µs → 84.4 µs at 1400 / 700 / 350 |
| gap-overlap census | 0 of 800 chains descheduled; the 12 slowest were all running |
| five repeats, pooled | every chain-A hop moves 13-18% between runs, p=0.0005 |

**The pre-flight refuses, and each refusal has been watched firing.** It will not
choose CPUs for you — it prints the machine's real topology, marks which cores
are isolated, and stops, because phase 10's numbering must not be inherited and
`nproc` reports the calling shell's affinity mask rather than the machine (16
CPUs in sysfs here against `nproc`'s 13). It vetoes the same CPU twice, SMT
siblings, and different physical packages. It checks every tool's exit code
before reading the file that tool was told to write. It runs `cpu_jitter` on
exactly the two chosen cores and treats that as the decisive gate; it runs
`tsc_offset` on exactly the chosen pair and takes the *larger* of the method's
resolution and its estimate as the bound, so the figure quoted beside a hop is
never optimistic. It does a short dry run and refuses to spend the long one
unless **every hop took samples and chain B joined at least one fill** — a hop
with zero samples scored as a pass is the failure that already happened twice in
12.7. Afterwards it re-runs both probes and re-reads the clocksource, and a
machine that moved under the run fails it.

It also records using non-isolated cores when the machine has isolated ones:
here cpus 13/14 are isolated and measurably quieter than 2/6 — 23 and 42 gaps
over 100 µs against 170 and 167 — which is a five-fold difference nobody would
have found by reading the output.

**Blocked on bare metal: every number.** On this box the run completes, joins
cleanly, and is refused for five separate measured reasons, of which the
load-bearing one is that a cross-process hop comes out 1467× more negative than
the clock bound can explain.

**The tests run in `verify-local` and in CI.** `tests/test_tick_to_trade.cpp`
covers the machinery whose failure would make a timing silently wrong rather than
obviously absent: an arena that wrapped instead of dropping (a wrapped index
pairs one order's t₀ with another's t₃ and the result looks like a plausible
latency), a clamped histogram that looked like a large sample, a topology probe
that treated *absent* as *fine*, and the record layouts.

`tests/test_report_join.py` builds trace files **by hand with defects planted in
them** and requires the report to refuse each one — because a live run cannot
test a refusal. It produces whatever it produces, and a silently wrong join still
looks like a table of plausible latencies. The planted defects are stamps out of
order, an exchange record naming an order that was never sent, the same token
twice, a fill whose two independently-counted ordinals disagree, a chain that
stopped running mid-hop (which the census must **tag** — the test that it can
fire at all), and a run with no census (where p99.9 must not be printed).

It also reads a fixture the C++ test writes, parsing it with the report's own
struct format strings, so the two languages are checked **against each other**
rather than against two copies of a constant. That is what would catch a padding
change that the Python reader would otherwise go on unpacking into plausible
numbers.

**Built, 2026-08-28.** `scripts/phase12-8-figures.py` draws both figures from
the artifact — the per-sample decompositions at the p50 and p99 ranks, and the
tail-conditional mean — and `scripts/phase12-8-results.py` generates
`docs/phase12-8-results.md`. Both are wired into `verify-local`'s
tracked-files-only gate, so a stale figure or table fails the build rather than
waiting for a reader to notice.

Both figures are built from **single orders and conditional means, never from
per-hop percentiles**: the p99 of a sum is not the sum of the p99s, so a stacked
bar of per-hop p99s draws an order that never existed.

---

## 11. The bare-metal boots, 2026-08-28

Three, all on the same day and the same machine. The first ran at the powersave
governor and is kept as evidence rather than results. The second produced the
numbers. The third was taken only to tie those numbers to a commit — and found
that the gate meant to establish that could never have passed.

| | governor | trace | provenance | what it settled |
|---|---|---|---|---|
| 1 | `powersave` | v1 | — | that the machine holds a CPU; that the harness must SET the governor, not just print it |
| 2 | `performance` | v1 | dirty tree | the numbers: headline 8,169 ns p50 (`validation/tick-to-trade-boot2-2026-08-28.json`) |
| 3 | `performance` | v2 | `a0fdd2c`, verified | reproduction to 0.4%, and the write syscall isolated at 7,824 ns |

The third boot's headline came back 8,139 ns against the second's 8,169 — 0.4%
apart, on a different pair of cores, from a separately built kit. That is the
strongest statement in the phase, and it is one no single run could make.

**Both artifacts are committed, and for a while only one was.** Re-ingesting the
third boot wrote over `validation/tick-to-trade-baremetal.json`, which had held
the second, and the 8,169 above then appeared in no artifact anywhere in the
tree — the phase's strongest claim resting on a number a reader could not check.
That is exactly the defect this phase had already fixed once, in
`c5c178f` ("p99.9 was printed to a terminal and never written down"). The second
boot is kept separately now, as
`validation/tick-to-trade-boot2-2026-08-28.json`, and it is marked not-quotable
on its own terms: its kit was built from a dirty tree, which is what the third
boot existed to fix.

Ten repeats plus the sweeps, on the live USB. **The harness did its job and the
numbers are not usable**, for one reason, and the distinction is the point of
having a harness.

### 11.1 What it established

**The machine holds a CPU.** This is what the boot was for, and it is a property
of the machine rather than of the measurement, so it stands regardless of §11.2:

    VERDICT: this machine holds a CPU. Not one gap over 100 us on any CPU
    tried, across 20 s each -- 10 per second over 10 us, worst 27 us.

Against WSL2's ~3,000 gaps per second over 10 µs and a worst gap of 2.57 ms,
that is roughly **300× quieter**, and it clears the bar the design set: no gap
long enough to move a microsecond-scale tail.

**The runs are repeatable.** The headline moved 775 ns across ten runs on a
median of 38,435 — **2.0%**, against 13–18% under WSL2 — and eight of twelve
hops came back poolable where nine of twelve had shown a run effect. The
permutation test that refused to pool on WSL2 accepts most of this.

**The census fires clean.** 0 of 1,200 chains descheduled, and **no chain at all
above 3× the median**. The tight distribution is what a quiet machine looks like,
and the census could have said otherwise.

**The clock is fine.** `clocksource=tsc`, cross-core offset **unmeasurable**,
bounded at 50 ns against the method's own resolution.

### 11.2 What it did not establish: the CPU was at the powersave governor

    governor               powersave

Every hop came back 4–6× slower than the same code under WSL2 — `t0→t1` 8,342 ns
against ~1,300, the headline 38,435 against 8,845 — **uniformly, across hops
that share no code**. A uniform slowdown across unrelated paths is not a code
finding; it is the clock the code was running at. A live session defaults to
powersave and nothing set it otherwise.

The harness **printed the governor and quoted the numbers anyway**. That is the
defect: a latency measured at an unspecified CPU frequency is not a latency. It
now sets the performance governor itself, verifies the change took — writing to
sysfs can fail silently — and treats anything else as a reason not to quote,
including the absent case, since WSL2 has no cpufreq at all and is precisely the
machine that check exists to flag.

### 11.3 Two smaller things the boot found

**The auto-picker chose cpu0.** Legal — on this hardware cpu0's sibling is cpu8,
so 0 and 1 are distinct physical cores and every veto passed — and still wrong,
because cpu0 is where Linux points device interrupts by default, making it the
busiest core on an otherwise idle machine. It is now chosen last. The live boot
also had no `isolcpus`, so there were no isolated cores to prefer.

**The ingest pooled the sweeps with the repeats.** `run-12-8.sh` produces the ten
repeats *and* the multiplier sweep, the MTU sweep, the held-ack arm and the dry
run; the ingest globbed all seventeen and pooled them as one experiment. They are
deliberately different configurations — `--multiplier 500` and `2000` exist
*because* they should differ — so pooling them asks whether runs set up to differ
differ. The repeats are pooled alone now and every other configuration is
reported on its own. It also wrote its working tree to `/tmp`, which is wiped
between WSL invocations here, so the raw material vanished between two commands.

### 11.4 What the next boot needs

Nothing but a re-run: the same one command. The governor is set by the harness
now, cpu0 is avoided, and everything else about the first boot was clean. The
run takes ~25 minutes and the numbers from it will be at a known, verified clock.
