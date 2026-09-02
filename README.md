# itchbook

A C++20 library and toolset for NASDAQ TotalView-ITCH 5.0. It parses the raw binary feed, reconstructs the limit order book for one symbol or for every security in a trading day, and runs a queue-position-aware backtest on the result. A MoldUDP64 layer carries the feed over UDP and rebuilds the book from the wire, with gap detection and recovery. On the order-entry side there is a matching engine reachable over OUCH 4.2 and SoupBinTCP 3.00, so a strategy process and an exchange process can trade against each other across real sockets.

A Python reference implementation of the book is kept beside the C++ one. The two are run over the same bytes and compared, which is how correctness is established.

The library is header-only. Its only external dependency is zlib.

## What it does

Each layer can be used on its own.

| Layer | Headers | Purpose |
|---|---|---|
| Feed | `itch/` | Gzip stream reader, ITCH 5.0 message layouts, framing and parsing |
| Book | `book/` | Order book with a dense price array near the touch, an open-addressing reference map, intrusive FIFO levels and a slab allocator. `BookSet` holds one book per security for whole-day replay |
| Engine | `engine/` | Price-time matching and the order-entry gateway |
| Simulation | `sim/` | Four queue-position fill models, ledger, fees, latency, markouts, an Avellaneda-Stoikov maker and the backtest driver |
| Wire in | `mold/`, `pipe/` | MoldUDP64 framing, a sequencer for gaps and duplicates, and a lock-free single-producer ring between the receiver thread and the book thread |
| Wire out | `emit/`, `ouch/`, `soupbin/` | ITCH encoding and publication, plus OUCH 4.2 over SoupBinTCP for order entry |
| Replay | `replay/` | Splits a historical day into the part the exchange replays and the part a strategy sees |
| Recovery | `recover/`, `risk/` | Gap policy, book and strategy snapshots, halt tracking and a kill switch |

Under `python/` there are feed generators for running without market data, the reference book used as an oracle, and the analysis scripts that grade reconstructions and render figures. The Python side uses the standard library only.

## Requirements

- A C++20 compiler. CI builds with GCC and Clang on Ubuntu, and with Clang on macOS.
- CMake 3.20 or later.
- zlib.
- Python 3.9 or later for the generators, the reference implementation and the analysis scripts.

```bash
# macOS
brew install cmake zlib

# Ubuntu
sudo apt install build-essential cmake ninja-build zlib1g-dev
```

## Build

```bash
git clone https://github.com/KareemJandali/itchbook && cd itchbook
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The default build type is Debug, which turns on AddressSanitizer and UndefinedBehaviorSanitizer and runs roughly ten times slower. Use Release for any run you intend to time. A Release build accepts `-DNATIVE=ON` for `-march=native`. Passing `-DITCHBOOK_TSAN=ON` builds under ThreadSanitizer instead, which is how the ring buffer and the threaded pipeline are checked.

Run the unit tests and the property fuzzers:

```bash
ctest --test-dir build --output-on-failure
```

No market data is needed for the tests.

## Quick start

Generate a small synthetic feed and inspect it:

```bash
python3 python/make_sample.py data/raw/sample.gz
./build/itch_census data/raw/sample.gz
./build/itch_dump   data/raw/sample.gz 10
```

Reconstruct the book and write top-of-book snapshots on a fixed time grid:

```bash
./build/book_replay data/raw/sample.gz --symbol TEST \
    --snapshots data/sliced/TEST_book.csv --interval-ms 100
```

Run the same input through the Python reference book and compare the two outputs. Any difference is a bug in one implementation:

```bash
python3 python/make_queue_feed.py data/raw/day.gz --messages 1200000 --gap-ns 20000
./scripts/full-day-differential.sh data/raw/day.gz
```

Backtest a passive strategy under all four fill models in a single pass:

```bash
./build/queue_backtest data/raw/day.gz --strategy touch-maker --max-position 1000 \
    --json out.json
python3 python/analysis/fill_comparison.py out.json
```

## Working with real data

NASDAQ publishes sample TotalView-ITCH 5.0 days as gzip files named `MMDDYYYY.NASDAQ_ITCH50.gz`. A full day runs to several gigabytes, and the single-symbol tools expect a slice. `itch_slice` extracts one symbol and keeps the system events that bracket the session:

```bash
./build/itch_slice 12302019.NASDAQ_ITCH50.gz MSFT data/sliced/MSFT.gz
./build/book_replay data/sliced/MSFT.gz --snapshots out/MSFT_book.csv --interval-ms 100
```

`python/slice_symbol.py` does the same in pure Python when no build is available.

Every security in a day can be reconstructed in one process:

```bash
./build/book_replay 12302019.NASDAQ_ITCH50.gz --all-symbols --per-symbol out/all.csv
```

The script `scripts/real-data-run.sh` slices a symbol, builds Release, and runs the backtest measurements in one command. Nothing under `data/` or `out/` is committed, because both derive from licensed data.

## The closed loop

Two processes trade against each other over real sockets, with no shared memory. `exchange` replays a historical day, matches incoming orders against the book, and publishes ITCH over MoldUDP64. `strategy` receives that UDP feed, rebuilds the book from it, and sends orders back over OUCH 4.2 on a SoupBinTCP session.

```bash
./scripts/closed-loop-check.sh
```

Ordering between the two transports is the part that needs care. An order's ITCH `Add` and `Execution` may arrive on UDP before its OUCH acknowledgement arrives on TCP, so fills are parked and retired once the acknowledgement lands.

`scripts/tick-to-trade.sh` measures the path from arrival to wire, broken down per hop.

## Tools

Executables are built into `build/`. Each one prints its options when run without arguments.

| Tool | What it does |
|---|---|
| `itch_dump` | Print the first N decoded messages of a feed |
| `itch_census` | Count messages by type, with optional per-symbol and peak-live-order statistics |
| `itch_slice` | Extract one symbol from a full day into its own gzip file |
| `book_replay` | Reconstruct one symbol or all symbols, with snapshot CSV and JSON summary output |
| `book_bench` | Measure cycles per message on the book, with a latency histogram |
| `queue_sim` | Place hypothetical orders into a replayed book and report when each fill model would have filled them |
| `queue_backtest` | Run a strategy under the four fill models, with fees, latency and kill-switch limits |
| `calibrate_intensity` | Fit the fill intensity function from the backtester's own fills |
| `as_experiment` | Compare a naive maker against the Avellaneda-Stoikov maker on one symbol-day |
| `latency_sweep` | Repeat a backtest across a ladder of order and cancel latencies |
| `mold_wrap`, `mold_replay`, `mold_damage` | Wrap a feed in MoldUDP64 packets, replay it, and inject gaps, duplicates and reordering |
| `mold_replay_udp`, `wire_to_book` | Send packets over UDP at a chosen rate, and rebuild the book on the receiving side |
| `exchange`, `strategy` | The two halves of the closed loop |
| `split_replay_gate` | Check that splitting a day between replay and live paths preserves the book |
| `restart_check` | Confirm that a book restored from a snapshot matches one replayed from the start |
| `tsc_offset`, `cpu_jitter` | Measure the time-stamp counter offset between cores, and the scheduling jitter of the machine taking a measurement |

## Tests

Unit tests under `tests/` cover each layer. Three property fuzzers run a bounded number of random sequences under `ctest` and accept `--iterations` for a longer soak. The differential tests compare the C++ book against the Python reference over a whole trading day, and `python/analysis/adversarial.py` grades the pipeline's behaviour under packet damage.

`scripts/verify-local.sh` runs the fast part of the CI gate on a laptop, building with both compilers and running the unit tests under each.

## Layout

```
include/itchbook/   public headers, grouped by layer as in the table above
tools/              command-line executables
python/
  reference/        the Python oracle: parser, book, replay driver, queue models
  analysis/         grading, differential and figure scripts
  make_*.py         synthetic feed generators
tests/              unit tests, property fuzzers and differential tests
bench/              benchmark harness, A/B comparison and the regression gate
scripts/            end-to-end runs, report generation and CI gates
validation/         committed reconstruction records and vendor reference data
docs/               design notes, results and the paper
data/  out/         gitignored working directories
```

## Documentation

Design notes, measured results and write-ups are under [`docs/`](docs/).

- [`docs/build-plan.md`](docs/build-plan.md) and [`docs/build-plan-9-12.md`](docs/build-plan-9-12.md) set out the phases of the project and their acceptance criteria.
- The `phase*-results.md` files record what each phase measured, on what data, and by what method.
- [`docs/paper/as-on-itch.md`](docs/paper/as-on-itch.md) is the paper on the Avellaneda-Stoikov maker with fill-model uncertainty bands. The referee report and the response to it are in [`docs/paper/review/`](docs/paper/review/).
- [`docs/writing/what-synthetic-data-hides.md`](docs/writing/what-synthetic-data-hides.md) collects notes on what changed once the tools met real data.
- [`validation/README.md`](validation/README.md) lists the reconstructions graded against a vendor's daily bars.

## License

MIT. See [LICENSE](LICENSE).
