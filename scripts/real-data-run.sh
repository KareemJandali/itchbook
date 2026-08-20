#!/usr/bin/env bash
#
# real-data-run.sh — the phase 6 numbers, on a real NASDAQ day.
#
#   ./scripts/real-data-run.sh 12302019.NASDAQ_ITCH50.gz MSFT
#
# Everything in docs/phase6-results.md was produced on a synthetic feed, which
# is enough to show the machinery measures what it claims to and not enough to
# say anything about a real market. This runs the same four measurements on a
# real day and writes the tables to out/real/ so they can be pasted into the
# results doc.
#
# Two things it does that are easy to forget by hand:
#
#   * It SLICES first. queue_backtest does not filter by symbol, so pointing it
#     at a full day would interleave 8,000 symbols into one book and produce
#     confident nonsense. itch_slice keeps the 'S' system events along with the
#     target symbol, so the session still opens.
#   * It builds RELEASE. The default Debug build has ASan and UBSan on and runs
#     roughly ten times slower, which on 1.2M messages is the difference between
#     a coffee and an afternoon.
#
set -euo pipefail

RAW="${1:-}"
SYMBOL="${2:-MSFT}"
SAMPLES="${3:-50}"

if [[ -z "$RAW" ]]; then
    echo "usage: $0 <full-day.NASDAQ_ITCH50.gz> [SYMBOL] [LEAVE_ONE_OUT_SAMPLES]" >&2
    echo "" >&2
    echo "  SYMBOL   defaults to MSFT" >&2
    echo "  SAMPLES  defaults to 50. Each sample replays the whole slice once," >&2
    echo "           so 200 is worth doing but is not a thirty-second job." >&2
    exit 2
fi
if [[ ! -f "$RAW" ]]; then
    echo "error: no such file: $RAW" >&2
    exit 1
fi

cd "$(dirname "$0")/.."
OUT="out/real"
mkdir -p "$OUT" data/sliced

echo "=== building (Release) ==="
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j >/dev/null
BIN=build-release

SLICE="data/sliced/${SYMBOL}.gz"
if [[ ! -f "$SLICE" ]]; then
    echo
    echo "=== slicing $SYMBOL out of $RAW ==="
    "$BIN/itch_slice" "$RAW" "$SYMBOL" "$SLICE"
else
    echo
    echo "=== reusing $SLICE (delete it to re-slice) ==="
fi

echo
echo "=== headline: four fill models, one pass ==="
"$BIN/queue_backtest" "$SLICE" --strategy touch-maker --max-position 1000 \
    --json "$OUT/$SYMBOL.json" | tee "$OUT/$SYMBOL-backtest.txt"

echo
echo "=== P&L by fill model ==="
python3 python/analysis/fill_comparison.py "$OUT/$SYMBOL.json" \
    --svg "$OUT/$SYMBOL-fills-total.svg" | tee "$OUT/$SYMBOL-fills.txt"
python3 python/analysis/fill_comparison.py "$OUT/$SYMBOL.json" --metric shares \
    --svg "$OUT/$SYMBOL-fills-shares.svg" --no-ascii

echo
echo "=== adverse selection ==="
# The one to watch. On the synthetic feed the drift is POSITIVE, because that
# price process mean-reverts. A real day should be negative and should worsen
# from 100ms to 1s: that is being picked off, and it is the thing this phase
# exists to measure.
python3 python/analysis/markout.py "$OUT/$SYMBOL.json" \
    --svg "$OUT/$SYMBOL-markout.svg" | tee "$OUT/$SYMBOL-markout.txt"

echo
echo "=== latency sensitivity ==="
"$BIN/latency_sweep" "$SLICE" --strategy touch-maker --max-position 1000 \
    --csv "$OUT/$SYMBOL-latency.csv" >/dev/null
python3 python/analysis/latency_sweep.py "$OUT/$SYMBOL-latency.csv" \
    --svg "$OUT/$SYMBOL-latency-pnl.svg" \
    --shares-svg "$OUT/$SYMBOL-latency-shares.svg" | tee "$OUT/$SYMBOL-latency.txt"

echo
echo "=== external check: shadow $SAMPLES real orders ==="
# Replays the slice once per sample, so this is the slow step by a wide margin.
python3 python/analysis/leave_one_out.py "$SLICE" --binary "$BIN/queue_sim" \
    --samples "$SAMPLES" --partial-only | tee "$OUT/$SYMBOL-oracle.txt"

echo
echo "=== done ==="
echo "tables and figures in $OUT/"
echo "nothing under data/ or out/ is committed — both are gitignored."
