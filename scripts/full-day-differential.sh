#!/usr/bin/env bash
#
# full-day-differential.sh — phase 3's done-condition, at the stated scale.
#
#   ./scripts/full-day-differential.sh data/sliced/MSFT.gz
#
# The plan says: "bit-identical output to the Python oracle across a full
# trading day. Not 'close.' Identical."
#
# Every differential run before this one used a sample feed or a generated
# adversarial one — tens of thousands of messages. Those catch logic errors,
# and they cannot catch anything that only appears after hours of session: a
# counter that drifts, a level that goes stale, an accumulator that overflows,
# a code path reached only by a message type that turns up twice a day. A full
# day is 1.2 million messages for one liquid name, and the claim was about a
# full day.
#
# Two comparisons, because they fail in different ways:
#
#   * The SNAPSHOT CSV — the top N levels of both sides, sampled through the
#     session. Diverge anywhere and the files differ from that row on.
#   * The DAILY SUMMARY — volume, OHLC, VWAP, resting orders and shares at the
#     close. A book can produce identical snapshots at every sampled instant
#     and still have miscounted between them; the summary is cumulative and
#     catches exactly that.
#
set -euo pipefail

FEED="${1:-}"
INTERVAL_MS="${2:-1000}"
LEVELS="${3:-10}"

if [[ -z "$FEED" ]]; then
    echo "usage: $0 <single-symbol-slice.gz> [INTERVAL_MS] [LEVELS]" >&2
    echo "" >&2
    echo "  slice a symbol first:  ./build/itch_slice <full-day.gz> MSFT out.gz" >&2
    exit 2
fi
[[ -f "$FEED" ]] || { echo "error: no such file: $FEED" >&2; exit 1; }

cd "$(dirname "$0")/.."
OUT="out/differential"
mkdir -p "$OUT"

# Reuse a build if there is one — CI already has the sanitizer build compiled
# and rebuilding the project to run a comparison is a waste of a CI minute.
# ITCHBOOK_BUILD overrides; otherwise prefer ./build, then build Release.
BIN="${ITCHBOOK_BUILD:-}"
if [[ -z "$BIN" ]]; then
    if [[ -x build/book_replay ]]; then
        BIN=build
    else
        echo "=== building (Release) ==="
        cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build build-release -j >/dev/null
        BIN=build-release
    fi
fi
echo "using $BIN/book_replay"

echo
echo "=== the Python oracle ==="
time python3 python/reference/replay.py "$FEED" \
    --snapshots "$OUT/py.csv" --interval-ms "$INTERVAL_MS" --levels "$LEVELS" \
    --json "$OUT/py.json" --quiet

echo
echo "=== the C++ book ==="
time "$BIN/book_replay" "$FEED" \
    --snapshots "$OUT/cpp.csv" --interval-ms "$INTERVAL_MS" --levels "$LEVELS" \
    --json "$OUT/cpp.json" --quiet

echo
echo "=== snapshots: bit-identical, or not ==="
python3 python/analysis/book_diff.py "$OUT/py.csv" "$OUT/cpp.csv"
echo "rows compared: $(( $(wc -l < "$OUT/py.csv") - 1 ))"

echo
echo "=== daily summary ==="
python3 - "$OUT/py.json" "$OUT/cpp.json" <<'PYEOF'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
# The intersection: everything both implementations report about the BOOK.
# `symbol` and `session_end_ns` are the Python driver's own bookkeeping and
# say nothing about whether the two books agree, so they are named rather
# than counted.
keys = sorted(set(a) & set(b))
only_py = sorted(set(a) - set(b))
only_cpp = sorted(set(b) - set(a))
bad = []
width = max(len(k) for k in keys)
for k in keys:
    x, y = a[k], b[k]
    if x != y:
        bad.append(k)
    print(f"  {k:<{width}}  {str(x):>18}  {str(y):>18}  {'' if x == y else '<-- DIFFER'}")
if only_py or only_cpp:
    print(f"\n  (driver-only fields, not compared: "
          f"{', '.join(only_py + only_cpp)})")
if bad:
    print("\nFAIL: the summaries disagree on " + ", ".join(bad))
    raise SystemExit(1)
print(f"\n{len(keys)} summary fields identical")
PYEOF

echo
echo "=== done: bit-identical across a full trading day ==="
