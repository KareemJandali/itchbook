# itchbook

A limit-order-book reconstructor, matching engine, and queue-position-aware
backtester built from raw **NASDAQ TotalView-ITCH 5.0** binary data — in C++20.

> **Status:** Phase 2 (Python reference book). Phase 1 is done — the gzip
> streaming `Reader`, the framing + dispatch `Parser`, and the `itch_dump` /
> `itch_census` / `itch_slice` tools build, run, and are covered by CI. The
> Python oracle now parses every modelled message type and reconstructs the
> book, with 54 tests in CI. **Not yet validated against real market data** —
> see [Validation](#validation) for the check that closes Phase 2. See
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
  book/ engine/     phases 3 & 5 (order book, matching engine) — stubs
  sim/ risk/        phases 6 & 7 (queue backtester, risk) — stubs
tools/              itch_dump, itch_census, itch_slice  (Phase 1 deliverables)
python/
  make_sample.py    synthetic spec-shaped feed, so you can run without a download
  reference/        the Phase 2 oracle: slow, obvious, and correct
    parser.py       framing + one decoder per message type
    book.py         {price: [refs]} + {ref: order}; the seven mutating handlers
    replay.py       driver — snapshot CSV + daily volume/OHLC/VWAP
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

Run the tests with:

```bash
ctest --test-dir build --output-on-failure                  # C++
python3 -m unittest discover -s tests -p 'test_*.py' -v     # Python oracle
```

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
done-condition and it has **not been run yet**:

```bash
# slice one mid-liquidity symbol out of a real day, then reconstruct it
./build/itch_slice data/raw/<day>.gz MSFT data/sliced/MSFT.gz
python3 python/reference/replay.py data/sliced/MSFT.gz \
    --snapshots data/sliced/MSFT_book.csv --interval-ms 1000
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
