# itchbook

A limit-order-book reconstructor, matching engine, and queue-position-aware
backtester built from raw **NASDAQ TotalView-ITCH 5.0** binary data — in C++20.

> **Status:** Phase 1 (harness). The gzip streaming `Reader`, the framing +
> dispatch `Parser`, and the `itch_dump` / `itch_census` / `itch_slice` tools
> build, run, and are covered by CI. See [`docs/build-plan.md`](docs/build-plan.md)
> for the full eight-phase roadmap.

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
python/             make_sample.py + reference oracle (Phase 2)
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

## License

MIT — see [LICENSE](LICENSE).
