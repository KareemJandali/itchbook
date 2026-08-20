# itchbook

A limit-order-book reconstructor, matching engine, and queue-position-aware
backtester built from raw **NASDAQ TotalView-ITCH 5.0** binary data — in C++20.

> **Status:** Phases 1–7 complete. Every claim below is measured on a real
> NASDAQ trading day — MSFT, 30 December 2019, 1,221,484 messages — not on a
> generated feed.
>
> **Correct.** The C++ book and an independent Python oracle agree byte for
> byte across **61,228 snapshot rows and 22 summary fields**, VWAP to ten
> decimal places, zero unknown order references. The reconstruction matches
> **Databento's published daily bar exactly** — volume, open, high, low, close,
> to the share and the cent. See [Validation](#validation).
>
> **Fast.** **1.73× fewer cycles per message** (43.9M msg/s), traced by
> hardware counter to the pool's slab allocation rather than anything on the
> hot path: the page-fault count matches the removed 41.9 MB slab to 99.5%. Two
> optimisations the plan predicted measured flat. See [`bench/`](bench/).
>
> **Honest about fills.** A symmetric maker at the touch **loses money in all
> four fill models**, with markouts negative at 100 ms, 1 s and 10 s — it is
> being picked off. Shadowing 200 real orders that were pulled part-filled puts
> the truth inside `[pessimistic, optimistic]` **200 times out of 200**, with
> the reference-resolving model exact on every one.
> See [`docs/phase6-results.md`](docs/phase6-results.md).
>
> **Never silently wrong.** Twelve scenarios of packet damage over that day —
> drops, duplicates, reordering, truncation, a 14:00 outage, an injected halt —
> and in none of them does the system report a trusted book that differs from
> the truth. The grader is proven able to fail.
> See [`docs/phase7-results.md`](docs/phase7-results.md).

## What it's for

Four things this project sets out to prove, in priority order:

1. **Correct binary-protocol handling** — parse a real exchange feed and
   reconstruct book state that matches ground truth exactly.
2. **Empirical performance** — measure, find the bottleneck, fix it, measure
   again, and explain each speedup with a hardware counter.
3. **Market-microstructure understanding** — queue position, adverse selection,
   the gap between a fill you'd *actually* get and one you'd *like* to have.
4. **Systems that fail safely** — gap detection, resync, kill switches.

## Layout

```
include/itchbook/   public headers (this is a library, not an app)
  itch/             reader (gzip stream), messages (field offsets), parser (framing)
  bench/            rdtsc timing and latency percentiles
  book/             order (40 bytes), level (intrusive FIFO), pool (slab),
                    book (dense tick array + open-addressing ref map),
                    dispatch (the ITCH -> book seam)
  engine/           order types, states, and price-time matching
  sim/ risk/        phases 6 & 7 (queue backtester, risk) — stubs
tools/              itch_dump, itch_census, itch_slice, book_replay, book_bench
python/
  make_sample.py    synthetic spec-shaped feed, so you can run without a download
  fuzz_feed.py      adversarial feed generator, for the differential test
  make_bench_feed.py  feed with a real day's message mix, for benchmarking
  reference/        the Phase 2 oracle: slow, obvious, and correct
    parser.py       framing + one decoder per message type
    book.py         {price: [refs]} + {ref: order}; the seven mutating handlers
    replay.py       driver — snapshot CSV + daily volume/OHLC/VWAP
  analysis/
    book_diff.py    diffs two snapshot CSVs — the differential test
    validate.py     grades a reconstruction against Databento — Phase 2's gate
tests/  bench/      unit/property tests, Google Benchmark
data/               gitignored — raw .gz feeds and per-symbol slices
```

## Build

Requires a C++20 compiler, CMake ≥ 3.20, and zlib.

```bash
# macOS:  brew install cmake zlib
# Ubuntu: sudo apt install build-essential cmake ninja-build zlib1g-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # Debug turns on ASan + UBSan
cmake --build build
ctest --test-dir build --output-on-failure     # run the unit tests
```

For a release build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` (add
`-DNATIVE=ON` for `-march=native`).

## Run it

You don't need a multi-GB NASDAQ download to try the tools. Generate a small,
spec-shaped synthetic feed first:

```bash
python3 python/make_sample.py data/raw/sample.gz

./build/itch_census data/raw/sample.gz            # message-type histogram
./build/itch_dump   data/raw/sample.gz 10         # first 10 messages, decoded
./build/itch_slice  data/raw/sample.gz TEST data/sliced/TEST.gz   # per-symbol extract
```

Then reconstruct the book with the Python reference implementation:

```bash
python3 python/reference/replay.py data/raw/sample.gz --symbol TEST \
    --snapshots data/sliced/TEST_book.csv --interval-ms 5
```

It prints the daily summary — volume (split into displayed, hidden `P` and cross
`Q`), OHLC, VWAP, the resting book, and a crossed-book check that should always
say `no` — and writes periodic top-of-book snapshots to CSV.

Snapshot rows land on a fixed grid of multiples of `--interval-ms`, anchored at
midnight, starting at the first grid point after the first message; each row is
the state as of just before the first message at or past that point. The columns
follow LOBSTER's orderbook layout (`ask_px_1,ask_sz_1,bid_px_1,bid_sz_1,...`,
with LOBSTER's dummy fills for levels a thin book doesn't reach) so a slice of
this CSV diffs straight against a LOBSTER file. Phase 3's C++ `book_replay` must
emit a byte-identical file.

The C++ book reconstructs the same thing from the same feed:

```bash
./build/book_replay data/raw/sample.gz --symbol TEST \
    --snapshots data/sliced/TEST_book_cpp.csv --interval-ms 5

python3 python/analysis/book_diff.py \
    data/sliced/TEST_book.csv data/sliced/TEST_book_cpp.csv
```

Run the tests with:

```bash
ctest --test-dir build --output-on-failure                  # C++
python3 -m unittest discover -s tests -p 'test_*.py' -v     # Python oracle
python3 tests/differential.py --binary build/book_replay    # C++ vs oracle
```

## Architecture

The parser knows nothing about books; the book knows nothing about strategies.
`dispatch.hpp` is the only file that knows both, and it mirrors the oracle's
`apply()` case for case so the two can be read side by side.

Three structures carry the book, and the choice for each is the point:

| Problem | Choice | Why |
|---|---|---|
| `ref -> Order*` | open addressing, linear probing, backward-shift deletion | every E/C/X/D/U carries only a reference, so this is the hottest lookup in the program. A node-based map costs a cache miss per message; tombstones would rot as a day's worth of orders is deleted |
| FIFO at a price | intrusive doubly-linked list | cancel-by-reference is `unlink()` — two pointer writes, no search, no allocation |
| `price -> level` | dense array indexed by tick offset | the few levels near the touch are hit millions of times and stay in L1. Off-band and off-tick prices fall through to a cold `std::map` |
| best bid / ask | index cursor per side | never scanned for; the touch moves one tick at a time, so walking outward is amortised O(1) |

`Order` is 40 bytes with a `static_assert` to keep it that way — that assert is
what will tell you the field reordering in phase 4 actually did something.
Orders come from a slab allocator in fixed chunks that are never reallocated, so
every pointer the levels hold stays valid.

## Performance

```bash
python3 python/make_bench_feed.py data/raw/bench.gz --messages 1000000
python3 bench/compare.py ./build/book_bench data/raw/bench.gz
```

`book_bench` reports per-message-type latency percentiles from `rdtsc`, plus an
uninstrumented throughput figure. `bench/compare.py` pins to a core and
interleaves variants, because without both this machine's run-to-run noise
(~19%) is large enough to invent a speedup that isn't there — which it did, on
the first attempt. [`bench/README.md`](bench/README.md) has the numbers, the
mechanism, and the two predicted optimisations that measured flat.

## Matching engine

The book replays the market. The engine puts *your* orders into it and decides
what trades.

```cpp
Matcher m;
m.submit({.id = 1, .side = Side::Sell, .price = 100'0000, .quantity = 100});
Result r = m.submit({.id = 2, .side = Side::Buy, .price = 100'5000, .quantity = 100});
// r.filled == 100, and the fill printed at 100'0000 — the resting order's price
```

Limit, market, IOC, FOK, iceberg, stop and stop-limit; self-trade prevention in
cancel-newest, cancel-oldest and cancel-both; and a state machine where an
illegal transition asserts rather than quietly corrupting the share count.

Three rules carry most of the behaviour, and none of them is arbitrary:

- **A fill prints at the resting order's price, never the incoming one.** The
  order that was there first set the terms, which is why price improvement goes
  to the taker.
- **Within a price, oldest trades first.** This is the "time" in price-time
  priority, and the reason phase 6 exists at all: your place in that queue
  decides whether you are filled.
- **An iceberg's next slice joins the back of the queue.** Hiding size costs
  priority. If it did not, everyone would hide everything.

### Property fuzzing

```bash
./build/fuzz_matcher --iterations 1000000 --seed 1
```

Random order sequences, with four invariants checked after every operation:
the book is never crossed; shares are conserved per order and in aggregate;
terminal orders hold nothing; and price-time priority holds, checked two ways —
fills at a price come in non-decreasing arrival order, and the front of the
book's queue is always the lowest-sequence order resting there.

That last check compares two mechanisms that share no code: the book maintains
its queue as an intrusive linked list, while arrival sequences are assigned by
the engine and never touched by the book. A bug in either shows up as a
disagreement.

Run to date: **1,000,000 sequences (135M operations) on seed 1, plus 500,000
each on seeds 2–5** — no invariant violated. CI runs a million on every push.

Input is a byte buffer decoded into operations, so the same file runs under
libFuzzer where its runtime is available:

```bash
clang++ -fsanitize=fuzzer,address -DITCHBOOK_LIBFUZZER \
    -Iinclude tests/fuzz/fuzz_matcher.cpp -o fuzz_libfuzzer
```

## Queue-position backtesting

A public feed tells you a cancel happened at your price. It does not tell you
whether it was ahead of your order or behind it. Ahead means you moved up the
queue; behind means you did not, and nothing in the data resolves it. So the
backtester runs four models over one book in one pass and reports the range:

- **naive** — filled whenever anything trades at your price. What most
  backtests do, and it is the number to distrust.
- **optimistic** — every cancel at your level was ahead of you.
- **pessimistic** — every cancel was behind you; only executions advance you.
- **mbo** — resolve the reference and know. Available because we reconstruct
  the book by order, not by level; it is the check that the band is a band.

```bash
./scripts/real-data-run.sh 12302019.NASDAQ_ITCH50.gz MSFT 50
```

MSFT, 30 December 2019, 1,221,484 messages:

```
model            fills    shares     P&L ($)     c/share   edge c/sh
naive            14995   962,594    -2753.09     -0.2860      0.2649
optimistic       12261   814,786    -3065.75     -0.3763      0.0841
mbo               9892   709,308    -3425.35     -0.4829     -0.1102
pessimistic       8548   655,232    -3154.44     -0.4814     -0.2179

naive reports $401.35 MORE than pessimistic
```

Markouts are negative at every horizon and worsen with it — −0.58 c/share at
10 s for naive, −0.23 for pessimistic. The maker is adversely selected, which
is why it loses. On a synthetic feed the same code reports naive at 3.53x
pessimistic's P&L and *positive* markouts; that generator mean-reverts, and
`docs/phase6-results.md` says which of the two sets of numbers means anything.

Also modelled: one-way latency, with queue position computed at arrival and
cancels that are late too, so a fill in the gap between deciding to pull and
actually pulling still happens; markouts at 100 ms / 1 s / 10 s with unresolved
fills counted rather than dropped; and the NASDAQ maker-taker schedule with
Section 31 and TAF, in integer micro-dollars.

The external check is the one that matters. `leave_one_out.py` replays the feed
**unchanged** with a simulated order standing exactly where a real order stood,
and grades the models against what that order actually filled. Over 200 real
MSFT orders that were pulled part-filled — the cases where the answer depends
on queue position — the truth fell inside `[pessimistic, optimistic]` 200 times
out of 200. `mbo` reproduced all 200 exactly; naive over-filled 89 and never
under-filled; pessimistic under-filled 96 and never over-filled. Full numbers,
figures and the limits in
[`docs/phase6-results.md`](docs/phase6-results.md).

## Recovery

A feed is not a file. NASDAQ ships the daily samples already de-MoldUDP'd —
every message present, in order, exactly once — so a book built only against
them has never met a dropped packet, and "it works on the sample file" is not
evidence that it would work on the wire.

So the framing goes back on. `mold_wrap` packages a plain ITCH file into real
MoldUDP64 packets; on a 200k-message feed that is ~4,900 datagrams of about
forty messages each, which is why packet loss differs in kind from message
loss. Then `mold_damage` does to them what a network does, and every scenario
is graded:

```bash
python3 python/analysis/adversarial.py data/sliced/MSFT.gz --build build
```

```
scenario             verdict     lost   gaps    dup  reord        state
clean                CORRECT        0      0      0      0      trusted
drop-1-in-100           SAFE    2,388     58      0   2263   recovering
duplicate-1-in-100   CORRECT        0      0   2388      0      trusted
reorder-1-in-100     CORRECT        0      0      0     56      trusted
disconnect-long      CORRECT   16,231      1      0     64      trusted
disconnect-to-end       SAFE        0      0      0      0       halted

CAUTIOUS=1  CORRECT=5  SAFE=4        (0 WRONG)
```

**CORRECT** means the book matched an undamaged replay and the system said so.
**SAFE** means it did not match and the system said *that*. **WRONG** — a book
that differs while claiming to be trusted — is the one outcome the phase exists
to make impossible, and CI fails on any occurrence.

On MSFT, 30 December 2019: 3 CORRECT, 7 SAFE, 0 WRONG, with the outage at a
real 14:00. Duplication and reordering are handled exactly — 12,199 duplicated
messages applied once, 270 reordered packets resolved without one false gap —
and a feed losing 269 packets halts rather than pretending to recover. There is no retransmission service to ask, so recovery
is rebuild-forward, and its guarantee is narrow and precise: the rebuilt book
contains **no wrong orders, only missing ones**. That holds unconditionally.
Whether it re-converges before the close does not: on a synthetic feed it
recovers from a 16,231-message hole to a byte-identical book, and on MSFT it
never converges at all, because a rebuild at 14:00 discards thousands of orders
that keep being cancelled for the rest of the day. The verdicts stay SAFE
throughout — the book differs and the system says so, which is the contract.

The harness also has to be able to fail. Set the convergence bar to a single
clean reference and three scenarios go WRONG; that run is in CI too, and must
fail. It found two real bugs, including a stream that *stopped* rather than
ended: 80,235 messages missing, every counter honestly zero, reported clean.
Full write-up in [`docs/phase7-results.md`](docs/phase7-results.md).

## Differential testing

The oracle exists to be disagreed with. Two independent implementations — a
slow, obvious Python one and the C++ one — run over the same bytes, so any
divergence is a bug in one of them.

The headline is a **full trading day**, because that is what the claim was
about and tens of thousands of messages cannot test it:

```bash
./scripts/full-day-differential.sh data/sliced/MSFT.gz
```

```
OK: 61228 snapshot rows identical
22 summary fields identical
```

MSFT, 30 December 2019, 1,221,484 messages. Every sampled instant of the book,
and every cumulative quantity between them — volume, notional, OHLC, VWAP to
ten decimal places. The two comparisons fail differently: two books can agree
at every instant a snapshot samples and still have miscounted in between, which
only the summary catches.

`tests/differential.py` covers the other direction — generated adversarial
feeds, requiring identical snapshot CSVs *and* identical summaries:

```bash
python3 tests/differential.py --binary build/book_replay --seeds 10
```

`fuzz_feed.py` aims at the paths a tidy sample never reaches — prices outside
the dense band and off the tick grid, levels emptied and refilled to make the
cursor walk, add/delete churn to work the ref map's deletion path, executions
larger than the resting size, references that never existed, and a second symbol
on another stock locate. Both implementations run over the same bytes, so any
divergence is a bug in one of them.

### On real data

```bash
# NASDAQ public sample (if the FTP is up):
wget ftp://anonymous:@emi.nasdaq.com/ITCH/01302019.NASDAQ_ITCH50.gz -P data/raw/

./build/itch_census data/raw/01302019.NASDAQ_ITCH50.gz
./build/itch_slice  data/raw/01302019.NASDAQ_ITCH50.gz MSFT data/sliced/MSFT.gz
```

A healthy day is mostly `A`/`D`/`X`/`E`, a few thousand `R` at the top, and a
handful of `S`. Anything wildly off means framing is broken. Alternatives if the
NASDAQ FTP is down: [LOBSTER](https://lobsterdata.com) sample files (also useful
as an oracle) or [Databento](https://databento.com) `XNAS.ITCH` free credits.

## Validation

Everything else in this repo checks the code against itself. The unit tests
check it against hand-derived numbers; the differential test checks the C++
against our own Python oracle. Both would pass happily if our reading of the
ITCH spec were wrong *in the same way in both implementations*. Only an outside
number settles that, and until one has been matched this project knows nothing.

That is phase 2's done-condition, and it is **met**: MSFT on 30 Dec 2019 matches
Databento's `XNAS.ITCH` daily bar exactly on all five fields. See
[`validation/`](validation/) for the record and the one subtlety that first run
turned up.

### Grading a reconstruction

```bash
# 1. slice one mid-liquidity symbol out of a real day and reconstruct it.
#    --utc-day bounds the replay to the window the oracle's bar covers; without
#    it the after-hours tail lands in the next bar and nothing lines up.
./build/itch_slice data/raw/<day>.gz MSFT data/sliced/MSFT.gz
python3 python/reference/replay.py data/sliced/MSFT.gz \
    --snapshots data/sliced/MSFT_book.csv --interval-ms 1000 \
    --utc-day 2019-12-30 --json data/sliced/MSFT.json

# 2. grade it against Databento's published bar for the same venue and day
pip install databento
export DATABENTO_API_KEY=db-...          # never commit this
python3 python/analysis/validate.py data/sliced/MSFT.json \
    --symbol MSFT --date 2019-01-30

# 3. confirm the C++ book agrees over the whole day
./build/book_replay data/sliced/MSFT.gz \
    --snapshots data/sliced/MSFT_book_cpp.csv --interval-ms 1000
python3 python/analysis/book_diff.py \
    data/sliced/MSFT_book.csv data/sliced/MSFT_book_cpp.csv
```

`validate.py --cost-only` prints what the query would bill before spending
anything, and `--oracle-json` re-runs the comparison offline against a fetch you
already paid for.

Passing means volume and OHLC match **exactly**. A book that is a few thousand
shares off is a book with a bug in it.

### Two things worth knowing before you start

**Databento is the oracle, not the input.** It serves normalised DBN, not raw
ITCH 5.0 binary, so it cannot feed this parser — the point is to prove *our*
parser reads the wire format correctly. The feed itself still has to come from
NASDAQ's public FTP (`emi.nasdaq.com/ITCH/`) or another source of the original
binary.

**Do not grade against a retail quote source.** Yahoo, Google and friends report
*consolidated* volume across every US venue and dark pool. This reconstruction
is NASDAQ-only, which is a fraction of that, so a consolidated figure will look
like a catastrophic failure when the code is fine. Databento's `XNAS.ITCH` bar
is built from the same feed we parse, which is exactly why it is the right
comparison. LOBSTER is an even stronger oracle in principle — same source,
comparable level for level rather than in aggregate — but it publishes
reconstructed output rather than the raw input, so it only works if you can get
a raw ITCH day for the same symbol *and* date as one of their samples.

### When it does not match

Debug roughly in this order:

| Symptom | Likely cause |
|---|---|
| Short a few percent, book looks right | `P`/`Q` volume — hidden and cross executions are real volume that never appears as a displayed order |
| Short by a lot | the locate filter is dropping messages, or the symbol resolved to the wrong locate code |
| Slightly over | non-printable `C` executions being counted; they move the book but not the volume |
| OHLC off but volume right | the opening and closing crosses, which set the official open and close |

## License

MIT — see [LICENSE](LICENSE).
