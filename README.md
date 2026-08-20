# itchbook

A limit-order-book reconstructor, matching engine, and queue-position-aware
backtester built from raw **NASDAQ TotalView-ITCH 5.0** binary data — in C++20.

> **Status:** Phase 3 (C++ parser + book). The C++ book reconstructs from the
> feed and agrees with the Python oracle byte for byte across adversarial
> replays — that differential test runs in CI on every push. **Not yet
> validated against real market data**, which is what actually closes phases 2
> and 3; see [Validation](#validation). See
> [`docs/build-plan.md`](docs/build-plan.md) for the full eight-phase roadmap.

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
  book/             order (40 bytes), level (intrusive FIFO), pool (slab),
                    book (dense tick array + open-addressing ref map),
                    dispatch (the ITCH -> book seam)
  engine/           phase 5 (matching engine) — stub
  sim/ risk/        phases 6 & 7 (queue backtester, risk) — stubs
tools/              itch_dump, itch_census, itch_slice, book_replay
python/
  make_sample.py    synthetic spec-shaped feed, so you can run without a download
  fuzz_feed.py      adversarial feed generator, for the differential test
  reference/        the Phase 2 oracle: slow, obvious, and correct
    parser.py       framing + one decoder per message type
    book.py         {price: [refs]} + {ref: order}; the seven mutating handlers
    replay.py       driver — snapshot CSV + daily volume/OHLC/VWAP
  analysis/
    book_diff.py    diffs two snapshot CSVs — the differential test
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

## Differential testing

The oracle exists to be disagreed with. `tests/differential.py` generates
adversarial feeds and requires the two implementations to produce identical
snapshot CSVs *and* identical summaries:

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

The synthetic sample proves the code is self-consistent. It does not prove the
protocol is understood correctly — only an external oracle does that, and until
one has been matched this project knows nothing. That check is Phase 2's
done-condition — and phase 3's, which asks for the same equality over a real
day rather than a synthetic one — and it has **not been run yet**:

```bash
# slice one mid-liquidity symbol out of a real day, then reconstruct it
./build/itch_slice data/raw/<day>.gz MSFT data/sliced/MSFT.gz
python3 python/reference/replay.py data/sliced/MSFT.gz \
    --snapshots data/sliced/MSFT_book.csv --interval-ms 1000

# ...and confirm the C++ book agrees over the whole day
./build/book_replay data/sliced/MSFT.gz \
    --snapshots data/sliced/MSFT_book_cpp.csv --interval-ms 1000
python3 python/analysis/book_diff.py \
    data/sliced/MSFT_book.csv data/sliced/MSFT_book_cpp.csv
```

Two ways to grade it, either of which is sufficient:

1. **NASDAQ's published daily summary** for that symbol and date — the reported
   volume, OHLC and VWAP must match the summary block this prints.
2. **A LOBSTER orderbook file** for the same symbol and day — drop the leading
   `ts` column and the snapshot rows must match level for level.

Pick a mid-liquidity name. Not AAPL (too many messages to eyeball when
debugging), not something illiquid (not enough book activity to be interesting).

## License

MIT — see [LICENSE](LICENSE).
