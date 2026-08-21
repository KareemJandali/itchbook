# Exchange Simulation — Build Plan

**Started:** August 2026
**Hard deadline:** September 2027 (full-time new grad applications open)
**Soft deadline:** March 2027 (fall-2027 stage applications — phases 1–4 must be public by then)

---

## 0. What you are building and why

A limit order book reconstructor, matching engine, and queue-position-aware
backtester, built from raw NASDAQ TotalView-ITCH 5.0 binary data.

Four things it must prove, in priority order:

1. **You can handle binary protocols correctly** — parse a real exchange feed and
   reconstruct state that matches ground truth exactly.
2. **You can make code fast, empirically** — measure, find the bottleneck, fix it,
   measure again, and explain each speedup with a hardware counter.
3. **You understand market microstructure** — queue position, adverse selection,
   the difference between a fill you'd actually get and a fill you'd like to have.
4. **You can build systems that fail safely** — gap detection, resync, kill switches.

Each phase has a **done-condition you can fail**. If a phase has no external
oracle, you are grading your own homework and the phase is worthless.

---

## 1. Repository structure

```
itchbook/
├── CMakeLists.txt
├── README.md                    # the deliverable — write it last, but keep notes from day 1
├── LICENSE                      # MIT
├── .github/workflows/ci.yml
├── .clang-format
├── .clang-tidy
│
├── include/itchbook/            # public headers (this is a library, not an app)
│   ├── itch/
│   │   ├── messages.hpp         # message layouts + field offsets
│   │   ├── parser.hpp           # framing + dispatch
│   │   └── reader.hpp           # gzip streaming source
│   ├── book/
│   │   ├── order.hpp            # intrusive order node
│   │   ├── level.hpp            # price level (FIFO)
│   │   ├── book.hpp             # the book
│   │   └── pool.hpp             # arena allocator
│   ├── engine/                  # phase 5
│   │   ├── matcher.hpp
│   │   └── order_types.hpp
│   ├── sim/                     # phase 6
│   │   ├── queue_model.hpp
│   │   └── latency_model.hpp
│   └── risk/                    # phase 7
│       ├── limits.hpp
│       └── recovery.hpp
│
├── src/                         # implementation
├── tools/
│   ├── itch_dump.cpp            # phase 1 deliverable
│   ├── itch_census.cpp          # message type histogram
│   ├── itch_slice.cpp           # symbol extractor — build this early, it saves months
│   └── book_replay.cpp          # main driver
│
├── python/
│   ├── reference/
│   │   ├── book.py              # the slow correct oracle
│   │   └── parser.py
│   └── analysis/
│       ├── latency_hist.py
│       ├── book_diff.py         # C++ vs Python differ
│       └── fill_comparison.py   # phase 6 headline chart
│
├── tests/
│   ├── test_parser.cpp
│   ├── test_book.cpp
│   ├── test_matcher.cpp
│   └── fuzz/
│       └── fuzz_book.cpp
│
├── bench/
│   └── bench_book.cpp           # Google Benchmark
│
└── data/                        # gitignored
    ├── raw/                     # the .gz files
    └── sliced/                  # per-symbol extracts
```

**Why a library layout and not an app layout:** the goal is for strangers to use
your ITCH parser. `include/itchbook/` with a clean public API is what makes that
possible. An app with everything in `src/` is a portfolio piece; a library is
something people can depend on.

---

## 2. Architecture

### 2.1 Data flow

```
  .gz file
     │
     ▼
  Reader          streams gzip, hands out byte spans, never materializes the file
     │
     ▼
  Parser          reads 2-byte length prefix, dispatches on message type byte
     │
     ▼
  Book            applies mutations, maintains best bid/ask
     │
     ├──────────► Matcher      (phase 5) — your own orders match against the book
     │
     └──────────► QueueModel   (phase 6) — where your passive order sits in FIFO
                       │
                       ▼
                  FillEngine → P&L
```

Parser knows nothing about books. Book knows nothing about strategies. The parser
should be usable standalone — that is what makes it a library other people want.

### 2.2 The order book — the core design decision

Three structures, and the choice for each matters:

**A. Order lookup: `order_ref → Order*`**

Every Execute / Cancel / Delete / Replace message carries only an order reference
number. You must find that order in O(1) or the whole thing is dead.

Do *not* use `std::unordered_map`. It stores nodes in separately-allocated buckets,
so every lookup is a pointer chase into cold memory — one cache miss per message,
on the hottest path in the program.

Use an **open-addressing hash table with linear probing** over a flat array. ITCH
order references within a day are roughly monotonic, so the low bits distribute
well. Start with `capacity = 2^22` slots and grow.

**B. Price level: FIFO queue of orders**

An **intrusive doubly-linked list** — the `next`/`prev` pointers live inside the
`Order` struct itself, not in a wrapper node. This is why C++ over Rust: cancel
by order reference becomes `unlink(order)` — two pointer writes, no search, no
allocation. With `std::list<Order>` you get a separate node allocation per order
and a pointer chase to reach the payload.

**C. Side: `price → PriceLevel`**

Options, worst to best:

- `std::map<price, Level>` — O(log n) plus red-black tree pointer chasing. No.
- Sorted `std::vector` — decent cache behavior, O(n) insert. Acceptable.
- **Dense array indexed by tick offset** — this is the answer.

Prices in ITCH are `Price(4)`: integers with 4 implied decimals. For a single
symbol on a single day, prices live in a narrow band. Allocate an array covering
roughly ±20% around the opening price:

```cpp
size_t index(int32_t price) const {
    return (price - base_price_) / tick_size_;
}
```

O(1) insert, O(1) lookup, and — the actual point — the few levels near the touch
get hammered millions of times a day and stay resident in L1. Keep a small
overflow `std::map` for prices outside the band (they're rare and cold).

**D. Best bid / ask**

Never scan for it. Keep an index cursor for each side. On a mutation, if the
current best level is now empty, walk outward one level at a time until you find a
non-empty one. Amortized O(1), because the touch moves by one tick almost always.

### 2.3 Order struct layout

```cpp
struct Order {
    uint64_t ref;         // 8   order reference number
    Order*   next;        // 8   intrusive FIFO
    Order*   prev;        // 8
    uint32_t shares;      // 4   remaining
    int32_t  price;       // 4   fixed point, 4 decimals
    uint32_t level_idx;   // 4   index into the side array
    uint8_t  side;        // 1   'B' or 'S'
    uint8_t  _pad[3];     // 3
};                        // = 40 bytes
```

40 bytes means you get one order per cache line with room to spare, and two orders
never straddle a line badly. Verify with `static_assert(sizeof(Order) == 40)`.
When you reorder fields in phase 4, this assert is what tells you it worked.

### 2.4 Allocation

Never `new` an `Order` on the hot path. Pre-allocate a **slab** of a few million
`Order` structs at startup and hand them out from a free list. Freed orders push
onto the free list head. Allocation becomes a pointer pop.

---

## 3. Environment setup

### 3.1 Toolchain

```bash
# Linux (you will need this by phase 4 — perf does not exist on macOS)
sudo apt install build-essential cmake ninja-build clang clang-format \
                 clang-tidy zlib1g-dev linux-tools-common linux-tools-generic

# verify perf works
perf stat ls
```

If you're on the MacBook, set up a Linux VM or a cheap cloud box now rather than
in month five. Do the phase 1–3 work anywhere; phase 4 needs Linux.

### 3.2 CMake skeleton

```cmake
cmake_minimum_required(VERSION 3.20)
project(itchbook CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -march=native)
endif()
```

Turn on sanitizers **now**. Your offset arithmetic will be wrong in week three, and
ASan will point at the exact line instead of you spending a day on a wrong book.

### 3.3 Data

```bash
# NASDAQ public FTP — sample ITCH 5.0 files
wget ftp://anonymous:@emi.nasdaq.com/ITCH/01302019.NASDAQ_ITCH50.gz -P data/raw/

# Spec (read section 4, the message formats table)
# NQTVITCHspecification.pdf from nasdaqtrader.com
```

If the FTP is down, alternatives: LOBSTER publishes sample message + orderbook
files built from the same NASDAQ source (useful as an oracle regardless), and
Databento gives free credits for `XNAS.ITCH`.

---

## 4. ITCH message reference

Only seven message types mutate the book. Everything else is metadata or feeds the
volume check.

| Type | Name | Effect on book |
|------|------|----------------|
| `S` | System Event | Market open/close markers. Bracket your session with these. |
| `R` | Stock Directory | ticker → stock locate code. Read these first. |
| `A` | Add Order | Insert order at back of its price level FIFO. |
| `F` | Add Order w/ MPID | Same as `A`, 4 extra bytes for the participant ID. |
| `E` | Order Executed | Reduce shares. If zero, unlink. Price from original order. |
| `C` | Order Executed w/ Price | Same, but at a different price. Check the printable flag. |
| `X` | Order Cancel | Partial cancel — reduce shares by the stated amount. |
| `D` | Order Delete | Full removal. Unlink and free. |
| `U` | Order Replace | Delete old ref, add new ref. **Loses queue priority.** |
| `P` | Trade (non-cross) | Hidden liquidity. Does **not** touch the book. |
| `Q` | Cross Trade | Opening/closing cross. Does **not** touch the book. |
| `H` | Stock Trading Action | Halts. Matters for phase 7. |

### Four traps that will cost you days each

1. **The length prefix.** The downloaded file is not a bare message stream. Each
   message is preceded by a 2-byte big-endian length. Read prefix → read that many
   bytes → dispatch on byte 0. Assert the prefix matches the spec length for that
   type; if it ever disagrees you have desynced, and you want to know immediately
   rather than 200MB later.

2. **Everything is big-endian.** You're on x86. Byteswap every integer field.

3. **Timestamps are 6 bytes.** Nanoseconds since midnight. There is no 6-byte
   integer type — assemble by hand:
   ```cpp
   uint64_t ts = 0;
   for (int i = 0; i < 6; ++i) ts = (ts << 8) | buf[off + i];
   ```

4. **`P` and `Q` count toward daily volume but never touch the book.** If your
   reconstructed daily volume is short by a few percent and the book itself looks
   right, this is why. Hidden and cross executions are real volume that never
   appeared as displayed orders.

**Read fields at explicit byte offsets. Do not `reinterpret_cast` to a packed
struct** — it's alignment and strict-aliasing UB, and after inlining it isn't
faster anyway.

---

## 5. Phases

### Phase 1 — Harness (2 weeks, by ~Sept 5)

1. Download one day. Stream through zlib; never gunzip to disk.
2. Read the spec's message format table. Print it. Keep it next to you.
3. Write `Reader`: gzip stream → byte spans.
4. Write the framing loop: length prefix → payload → type byte.
5. Write `itch_dump`: print type, timestamp, hex payload for the first N messages.
6. Write `itch_census`: count messages by type across the whole day. You should
   see mostly `A`/`D`/`X`/`E`, a few thousand `R` at the top, and a small fixed
   number of `S`. Anything wildly off means framing is broken.
7. Write `itch_slice`: read the `R` block, find your symbol's stock locate code,
   filter the whole file on a 2-byte header compare, write a small per-symbol file.

**Build the slicer before anything else in phase 2.** Otherwise every test run is
a multi-GB scan, you stop running the full day, and you stop noticing when you
break something. Slow feedback loops are what actually kill projects like this.

**Symbol choice:** mid-liquidity. Not AAPL (too many messages to eyeball when
debugging), not something illiquid (not enough book activity to be interesting).

**Done:** you can slice one symbol out of a full day, and the census looks
structurally sane.

---

### Phase 2 — Python reference book (3 weeks, by ~Sept 26)

Write the whole thing again in Python, slowly and obviously. Dicts everywhere.
No cleverness. This is not wasted work — it is your oracle for the next six months.

1. Parser in Python (`struct.unpack`, big-endian format strings).
2. Book as `{price: [list of orders]}` and `{ref: order}`.
3. Handle all seven mutating message types.
4. Emit book snapshots (top 10 levels) at fixed intervals to CSV.
5. Emit total daily volume, OHLC, VWAP.

**Done:** your reconstructed daily volume and OHLC for the symbol match NASDAQ's
published daily summary for that date — or match LOBSTER's published orderbook
file for the same symbol/day, level for level.

This is the moment the project becomes real. Until you've matched an external
number, you don't know anything.

---

### Phase 3 — C++ parser + book (6 weeks, by ~Nov 7)

1. `messages.hpp`: field offsets as `constexpr`, one accessor per field.
2. `parser.hpp`: framing, dispatch to handler callbacks (templated on handler type
   so it inlines — no virtual dispatch on the hot path).
3. `pool.hpp`: slab allocator + free list.
4. `order.hpp` / `level.hpp`: intrusive list, `static_assert` on `sizeof`.
5. `book.hpp`: dense tick array + open-addressing ref map + best bid/ask cursors.
6. `book_replay`: drives it, emits the same CSV format as the Python reference.
7. `python/analysis/book_diff.py`: diffs the two CSVs.

**Done:** bit-identical output to the Python oracle across a full trading day.
Not "close." Identical.

**Ship it here.** Tag v0.1, write the README, post it. A working ITCH parser and
book reconstructor is genuinely useful to other people on its own — this is the
part that gets stars and issues. Phases 5–6 are more interesting engineering but
far less reusable.

---

### Phase 4 — Performance (5 weeks, by ~Dec 12)

**Measure before you touch anything.** Baseline first, or you have no story.

1. `rdtsc`-based timing around each message handler. Store into a preallocated
   array; never allocate or print inside the measured region.
2. Compute p50 / p99 / p99.9 per-message latency. Save the baseline.
3. `perf stat ./book_replay` — record cache misses, branch mispredicts, IPC.
4. `perf record` / `perf report` — find the top three hotspots.
5. Optimize one thing. Re-measure both latency and the counter you were targeting.
6. Repeat.

Likely wins, roughly in order:
- Replacing `std::unordered_map` with open addressing (biggest single win)
- Order struct field reordering to fit the cache line
- Branch reordering: `A` and `D` are ~80% of messages; check those first
- Removing the `std::function`/virtual indirection from dispatch
- Prefetching the next order in the free list

**Done:** a before/after latency histogram, and for every speedup you can name the
hardware counter that moved. "It got faster" is not a done-condition. "Cache
misses dropped 60% because the order struct now fits in one line, and p99 went
from X to Y" is.

---

### Phase 5 — Matching engine (6 weeks, by ~Jan 30)

Now you inject *your own* orders into the book and match them.

1. Price-time priority matching.
2. Order types: limit, market, IOC, FOK, iceberg (displayed vs hidden quantity),
   stop / stop-limit.
3. Self-trade prevention (cancel-newest, cancel-oldest, cancel-both).
4. Order state machine: New → Accepted → PartiallyFilled → Filled / Cancelled /
   Rejected. Illegal transitions must assert.

**Done:** property-based fuzzing over a million random order sequences with these
invariants never violated:
- The book is never crossed (best bid < best ask)
- Total shares in = total shares out + shares resting
- Every order is in exactly one state
- FIFO priority is preserved within a price level

Use libFuzzer with a structured input decoder. This is the phase where fuzzing
earns its keep — hand-written tests will not find the ordering bugs.

---

### Phase 6 — Queue-position backtester (8 weeks, by ~Mar 27)

**This is the differentiator.** Everything before it is competent; this is what
makes an interviewer lean in.

The mechanic: when you place a passive order at price P, record the number of
shares already resting at that level. You are behind all of them. You only fill
after that many shares have executed *or cancelled* ahead of you.

The hard part — and the part worth writing about publicly:

> A public market data feed tells you a cancel happened at your price level. It
> does not tell you whether that cancel was **ahead of** or **behind** your order.
> Ahead means you moved up the queue. Behind means you didn't.

You cannot resolve this. So model both bounds:
- **Optimistic:** every cancel at your level was ahead of you.
- **Pessimistic:** every cancel was behind you; only executions advance you.

Report the range. A backtest that reports a P&L range with the queue assumption
stated is more credible than one that reports a single number.

Also build:
1. **Latency model.** Your order takes N microseconds to reach the exchange. The
   book moves in between. Make N configurable and show P&L sensitivity to it.
2. **Adverse selection measurement.** After you fill, where does the mid go over
   the next 100ms / 1s / 10s? If it consistently moves against you, you're being
   picked off — quantify it.
3. **Transaction costs.** Exchange fees, rebates for passive fills. Maker-taker
   changes the sign of many strategies.

**Done:** the headline chart. Same trivial strategy, three fill models — naive
touch-fill, optimistic queue, pessimistic queue — plotted P&L side by side. The
gap between naive and pessimistic is the entire point of the project.

Run a strategy you *know* is unprofitable and confirm it loses money. A backtester
that can't produce a loss is broken.

---

### Phase 7 — Risk and recovery (4 weeks, by ~Apr 24)

1. **Kill switch.** Trips on: position limit, notional limit, message rate limit,
   order-to-fill ratio, P&L drawdown. Each independently configurable.
2. **Sequence gap detection.** MoldUDP64 carries sequence numbers. Detect gaps.
3. **Snapshot resync.** On a gap, discard book state, request/replay a snapshot,
   resume. Do not silently continue with a corrupt book.
4. **Halt handling.** `H` messages. Book state across a halt and resume.
5. **State recovery.** Reconstruct position and open orders after a mid-day
   process restart.

**Done:** adversarial replay harness. Take a real trading day and inject:
dropped packets, reordered packets, duplicated packets, a mid-day disconnect at
14:00, a trading halt. The system must either produce correct state or halt
safely. Never silently wrong.

---

### Phase 8 — Write-up (shipped August 2026)

*Done. The README is the deliverable and it now leads with the verification
claim. The engineering phases that follow it are numbered 9-12 —
see [`build-plan-9-12.md`](build-plan-9-12.md), which supersedes the numbering
below and the September 2027 timeline in section 7.*

The README is the deliverable a recruiter actually sees. Structure:

1. One paragraph: what it is, one sentence on why it's hard.
2. **The verification claim, up front.** "Reconstructs NASDAQ's book for
   [symbol] on [date], validated against [oracle], across N million messages."
3. Latency histogram, with the hardware counters behind each optimization.
4. The queue-position fill comparison chart.
5. Architecture section — the dense tick array, the intrusive lists, and *why*.
6. Reproduction instructions that actually work on a clean machine.

Then: one technical blog post. Best candidates are the queue-position ambiguity
problem (novel and interesting to practitioners) or the cache-layout work with
before/after counters (concrete and verifiable). Post it. Engineers at these firms
read this material, and a post that lands gets you replies from people who work
there — which is worth more than any number of cold applications.

---

## 6. Testing strategy

| Level | Tool | What it catches |
|-------|------|-----------------|
| Unit | GoogleTest | Field offsets, byteswaps, single-message handling |
| Differential | Python oracle + `book_diff.py` | Logic divergence over a full day |
| External | LOBSTER / NASDAQ daily summary | Whether your understanding of the protocol is right |
| Property | libFuzzer | Ordering bugs, invariant violations |
| Adversarial | Packet-mangling replay | Recovery paths |
| Performance | Google Benchmark + perf | Regressions |

CI on every push: build with sanitizers, run unit + property tests, run the
differential test against a small committed sample slice.

---

## 7. Timeline

| Window | Phase | Public milestone |
|--------|-------|------------------|
| Aug 19 – Sep 5, 2026 | 1. Harness | — |
| Sep 6 – Sep 26 | 2. Python oracle | — |
| Sep 27 – Nov 7 | 3. C++ parser + book | **v0.1 released** |
| Nov 8 – Dec 12 | 4. Performance | **v0.2 + benchmark writeup** |
| Dec 13 – Jan 30, 2027 | 5. Matching engine | v0.3 |
| Jan 31 – Mar 27 | 6. Queue backtester | **v1.0 + blog post** |
| — | — | *Fall 2027 stage applications go out here* |
| Mar 28 – Apr 24 | 7. Risk + recovery | v1.1 |
| May – Sep 2027 | 8. Write-up, polish, outreach | *Full-time applications open* |

---

## 8. Running in parallel from day one

The project does not teach these, and they are what the screen tests:

- **C++ fundamentals.** Move semantics, RAII, templates, `constexpr`, the memory
  model. Read *Effective Modern C++*, then *C++ Concurrency in Action*.
- **DS&A in the extended-collaborative format.** Take one problem, solve it, then
  keep extending it as constraints change. That's the format these firms use.
  Speed-rep LeetCode does not prepare you for it.
- **CV rewrite.** Your current one reads as data analyst. Reframe in systems
  language where honest, and put this project at the top.

---

## 9. First session checklist

- [ ] `git init itchbook`, push empty repo with MIT license
- [ ] CMake skeleton building an empty binary with `-Werror` and sanitizers
- [ ] Download one ITCH day into `data/raw/`
- [ ] Download the ITCH 5.0 spec PDF, print the message table
- [ ] Write `Reader` — stream the gz, count total bytes
- [ ] Write the framing loop, print the first 100 message type bytes
- [ ] Confirm the type distribution looks like a market and not like noise
