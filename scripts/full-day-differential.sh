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
# Bring the chosen build up to date rather than trusting whatever binary is
# sitting there. ITCHBOOK_BUILD pointing at a tree compiled before this script
# existed is the obvious way to get a confusing failure, and an incremental
# no-op build costs a second.
if [[ -f "$BIN/CMakeCache.txt" ]]; then
    cmake --build "$BIN" --target book_replay -j >/dev/null
fi
echo "using $BIN/book_replay"

# ...and if it still cannot do what this script needs, say so plainly instead
# of letting the tool print a usage message into the middle of a comparison.
# Captured, not piped: book_replay exits non-zero on an unrecognised flag, and
# under `set -o pipefail` the pipeline reports THAT status rather than grep's,
# so a piped check reads as failure even when the flag is present. It did.
HELP_TEXT="$("$BIN/book_replay" --help 2>&1 || true)"
if ! printf '%s' "$HELP_TEXT" | grep -q -- '--json'; then
    echo "error: $BIN/book_replay does not support --json." >&2
    echo "       It predates this script. Rebuild it, or unset ITCHBOOK_BUILD" >&2
    echo "       to let this script configure a fresh Release build." >&2
    exit 1
fi

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
python3 python/analysis/summary_diff.py "$OUT/py.json" "$OUT/cpp.json"

echo
echo "=== done: bit-identical across a full trading day ==="
