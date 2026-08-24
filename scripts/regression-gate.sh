#!/usr/bin/env bash
#
# regression-gate.sh — the output of a reconstruction must not drift.
#
#   ./scripts/regression-gate.sh            # check against the committed baseline
#   ./scripts/regression-gate.sh --update   # re-record it, deliberately
#
# Phase 9 rewrote the inside of the book: the storage moved out from under it,
# every order gained a field, and messages now arrive through a router. None of
# that is supposed to change what a reconstruction produces, and "supposed to"
# is not a thing this repository accepts. So a known feed's output is recorded
# and compared byte for byte on every push.
#
# What is frozen, and why each one:
#
#   * the snapshot CSV      — the book's state through the session
#   * the summary JSON      — everything cumulative between those instants
#   * the per-symbol CSV    — the same feed through --all-symbols, so the
#                             multi-symbol path is guarded too, not only the
#                             one that existed before it
#
# The baseline is generated, not real. Licensed market data cannot live in a
# repository, so this gate rides on make_queue_feed with a fixed seed, and it
# checks the FEED's hash before it checks anything else. That distinction is
# the whole reason this is a script and not two diff commands: a generator
# whose output moved and a book whose output moved fail in the same place and
# mean opposite things, and being told which is the difference between a
# five-minute fix and an afternoon.
#
# The real-day equivalent is in validation/, which is where an MSFT baseline
# goes when the file is on the machine. It cannot run here.
set -euo pipefail

cd "$(dirname "$0")/.."

BASE="validation/regression"
UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

BIN="${ITCHBOOK_BUILD:-build}"
if [[ ! -x "$BIN/book_replay" ]]; then
    echo "=== building (Release) ==="
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build-release --target book_replay -j >/dev/null
    BIN=build-release
fi

# BRING THE CHOSEN BUILD UP TO DATE RATHER THAN TRUSTING WHATEVER IS THERE.
#
# A gate that reuses whichever binary happens to be sitting in build/ is a gate
# that can test code nobody is looking at. This one did: a WSL clone carried a
# book_replay from before --json existed, and the gate ran it. It failed loudly
# only because the flag would not parse -- had the stale binary merely BEHAVED
# differently, the gate would have gone green against old code, which is the
# failure this whole script exists to prevent.
#
# An incremental no-op build costs a second. scripts/full-day-differential.sh
# already did this; the other gates did not, and that is why this comment is
# now in three files.
if [[ -f "$BIN/CMakeCache.txt" ]]; then
    if ! cmake --build "$BIN" --target book_replay -j >/dev/null; then
        echo "error: $BIN failed to rebuild; the gate will not run a stale binary" >&2
        exit 1
    fi
fi

SEED=20260821
MESSAGES=60000
GAP_NS=20000
INTERVAL_MS=10
LEVELS=10
LOCATES=4

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

python3 python/make_queue_feed.py "$OUT/one.gz" --messages "$MESSAGES" --seed "$SEED" \
    --gap-ns "$GAP_NS" >/dev/null
python3 python/make_queue_feed.py "$OUT/many.gz" --messages "$MESSAGES" --seed "$SEED" \
    --gap-ns "$GAP_NS" --locates "$LOCATES" >/dev/null

# The feed's own hash, over the UNCOMPRESSED bytes: gzip's framing is not the
# thing under test and a different zlib would fail this for no reason.
hash_feed() {
    python3 -c "import gzip,hashlib,sys; print(hashlib.sha256(gzip.open(sys.argv[1],'rb').read()).hexdigest())" "$1"
}
{
    echo "one  $(hash_feed "$OUT/one.gz")"
    echo "many $(hash_feed "$OUT/many.gz")"
} > "$OUT/feeds.sha256"

"$BIN/book_replay" "$OUT/one.gz" --symbol TEST --snapshots "$OUT/snapshots.csv" \
    --interval-ms "$INTERVAL_MS" --levels "$LEVELS" --json "$OUT/summary.json" --quiet
"$BIN/book_replay" "$OUT/many.gz" --all-symbols --per-symbol "$OUT/per-symbol.csv" --quiet

FILES=(feeds.sha256 snapshots.csv summary.json per-symbol.csv)

if [[ "$UPDATE" == "1" ]]; then
    mkdir -p "$BASE"
    for f in "${FILES[@]}"; do cp "$OUT/$f" "$BASE/$f"; done
    echo "recorded a new baseline in $BASE/ — commit it with the reason it moved"
    exit 0
fi

for f in "${FILES[@]}"; do
    if [[ ! -f "$BASE/$f" ]]; then
        echo "error: no baseline at $BASE/$f. Record one with --update." >&2
        exit 1
    fi
done

# The feed first. If the generator moved, every other diff below is noise, and
# saying so is worth more than dumping three files of it.
if ! diff -q "$BASE/feeds.sha256" "$OUT/feeds.sha256" >/dev/null; then
    echo "THE FEED CHANGED, not the book." >&2
    echo >&2
    diff "$BASE/feeds.sha256" "$OUT/feeds.sha256" >&2 || true
    echo >&2
    echo "make_queue_feed.py produces different bytes for seed $SEED than it did when" >&2
    echo "this baseline was recorded — a change to the generator, or a Python whose" >&2
    echo "random sequence differs. The book's output below is not comparable until" >&2
    echo "that is resolved. Re-record with --update once you know which it was." >&2
    exit 1
fi

fail=0
for f in snapshots.csv summary.json per-symbol.csv; do
    if diff -q "$BASE/$f" "$OUT/$f" >/dev/null; then
        printf "  %-16s identical (%s lines)\n" "$f" "$(wc -l < "$OUT/$f")"
    else
        printf "  %-16s DIFFERS\n" "$f"
        diff "$BASE/$f" "$OUT/$f" | head -20
        fail=1
    fi
done

if [[ "$fail" != "0" ]]; then
    echo >&2
    echo "The reconstruction changed. If that was intended, say why in the commit" >&2
    echo "message and re-record with --update; if it was not, this is the bug." >&2
    exit 1
fi

echo
echo "=== unchanged: the same feed still reconstructs to the same bytes ==="
