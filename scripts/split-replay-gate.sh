#!/usr/bin/env bash
#
# split-replay-gate.sh — phase 12.1's gate.
#
#   ./scripts/split-replay-gate.sh                       # generated feeds only
#   ./scripts/split-replay-gate.sh <full-day.gz>         # ...and the real day
#
# The claim being gated: with zero strategy orders, the split replayer leaves
# the book exactly where the phase-9 path leaves it, after EVERY message, and
# the reference partition held throughout.
#
# Three detectors run, and they are not redundant. A mutation test (five
# plausible implementation mistakes, each applied to the replayer in turn)
# established which catches what:
#
#                              queue feed   bench feed   unit tests
#   drop the tape print            yes         yes          yes
#   walk the queue naively         NO          yes          yes
#   execute() for the remainder    yes         yes          yes
#   clamp print to resting size    NO          NO           yes
#   original_ref only on replace   NO          NO           yes
#
# The queue feed executes only the front order at the touch, so it never
# exercises the skip-historical-orders-ahead rule at all; the bench feed
# executes a randomly chosen resting order, which is wrong as a market and
# exactly right as a stress. Neither generated feed -- nor the real day --
# contains an execution larger than the order it names, so the over-execution
# path is reachable only from a hand-built message. That is why the unit suite
# is a gate component and not a formality.
#
# Exit codes: 0 pass, 1 failure, 2 usage, 3 refusal (the run could not
# guarantee its own preconditions, so its numbers mean nothing either way).
#
set -euo pipefail

cd "$(dirname "$0")/.."

DAY="${1:-}"
if [[ -n "$DAY" && ! -f "$DAY" ]]; then
    echo "error: no such feed: $DAY" >&2
    exit 2
fi

WORK="${ITCHBOOK_GATE_WORK:-out/phase12-1}"
mkdir -p "$WORK"

echo "=== building (Release) ==="
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j --target split_replay_gate test_split_replay >/dev/null
BIN=build-release

# ---- 1. the hand-built cases ------------------------------------------------
# First, because they are the only detector for two of the five mutations, and
# because a failure here is a defect in the replayer rather than a disagreement
# about a feed.
echo
echo "=== 1. unit suite (hand-built messages) ==="
"$BIN/test_split_replay"

# ---- 2. the generated feeds -------------------------------------------------
QUEUE="$WORK/queue.gz"
if [[ ! -f "$QUEUE" ]]; then
    echo
    echo "=== generating the queue feed (executions at the touch, front first) ==="
    python3 python/make_queue_feed.py --seed 7 --messages 400000 --locates 8 "$QUEUE" >/dev/null
fi

echo
echo "=== 2. queue feed — every execution at the front of the touch ==="
"$BIN/split_replay_gate" "$QUEUE" \
    --per-symbol-ref "$WORK/queue-ref.csv" --per-symbol-split "$WORK/queue-split.csv"

if [[ -f data/raw/bench.gz ]]; then
    echo
    echo "=== 3. bench feed — executions scattered across the book ==="
    "$BIN/split_replay_gate" data/raw/bench.gz \
        --per-symbol-ref "$WORK/bench-ref.csv" --per-symbol-split "$WORK/bench-split.csv"
else
    echo
    echo "!! data/raw/bench.gz is missing, so the ONE detector for the naive-queue-walk"
    echo "!! mistake among the feeds did not run. This is a weaker gate than it looks."
    echo "!! Regenerate it with python/make_bench_feed.py."
    exit 3
fi

# ---- 4. determinism, and the hash CI checks ---------------------------------
#
# Fixed input, byte-identical output. Twice, because a check that only compares
# against a stored constant cannot tell "the emitter is deterministic" from "the
# emitter is broken the same way every time"; and against the stored constant,
# because two identical runs cannot tell "correct" from "drifted together".
#
# make_sample.py is the input. It regenerates from a committed script with no
# seed to lose, it needs no licensed data, and its own docstring says it carries
# every message type the reference book models -- which is what makes a single
# hash meaningful rather than a hash of whichever types happened to appear.
SAMPLE="$WORK/sample.gz"
python3 python/make_sample.py "$SAMPLE" >/dev/null
"$BIN/split_replay_gate" "$SAMPLE" --emit "$WORK/emit-a.itch" >/dev/null
"$BIN/split_replay_gate" "$SAMPLE" --emit "$WORK/emit-b.itch" >/dev/null

echo
echo "=== 4. emitted-ITCH determinism ==="
if ! cmp -s "$WORK/emit-a.itch" "$WORK/emit-b.itch"; then
    echo "  FAIL: two runs over one input published different bytes"
    exit 1
fi
echo "  two runs over one input: byte-identical"

HASH=$(sha256sum "$WORK/emit-a.itch" | cut -d" " -f1)
STORED="validation/emitted-itch-sha256.txt"
if [[ -f "$STORED" ]]; then
    WANT=$(cut -d" " -f1 < "$STORED")
    if [[ "$HASH" != "$WANT" ]]; then
        echo "  FAIL: the emitted stream changed"
        echo "    stored $WANT"
        echo "    got    $HASH"
        echo "  If that change is intended, update $STORED and say why in the commit."
        exit 1
    fi
    echo "  hash matches $STORED"
else
    echo "  $HASH"
    echo "  (no stored hash yet -- writing one)"
    echo "$HASH  emitted by split_replay_gate --emit from python/make_sample.py" > "$STORED"
fi

# ---- 4b. an independent decoder reads what we published ---------------------
#
# P1 has one structural blind spot and it is worth naming rather than working
# around: the emitter writes at offsets taken from messages.hpp and the consumer
# reads at offsets taken from messages.hpp, so any error in this project's MODEL
# of the wire -- a field at the wrong offset, a length off by one -- cancels out
# and the round trip closes anyway. make_sample.py already states the same
# limitation about its own builders.
#
# The reference implementation in python/reference/ is a second, separately
# written decoder, and it is the project's standing answer to exactly this: the
# phase-2/3 differential exists because one implementation checking itself is
# not a check. So the published stream is handed to it, and its daily summary
# must match the summary it produces from the original feed.
echo
echo "=== 4b. the published feed, read by the Python reference decoder ==="
gzip -cf "$WORK/emit-a.itch" > "$WORK/emit-a.gz"
python3 python/reference/replay.py "$SAMPLE" --symbol TEST \
        --json "$WORK/py-original.json" --quiet >/dev/null
python3 python/reference/replay.py "$WORK/emit-a.gz" --symbol TEST \
        --json "$WORK/py-published.json" --quiet >/dev/null
if ! cmp -s "$WORK/py-original.json" "$WORK/py-published.json"; then
    echo "  FAIL: the reference decoder reads a different book from the published feed"
    diff "$WORK/py-original.json" "$WORK/py-published.json" | head -20
    exit 1
fi
echo "  identical daily summary from a decoder that is not the one that wrote it"

# ---- 5. the real day --------------------------------------------------------
if [[ -n "$DAY" ]]; then
    echo
    echo "=== 5. the real day: $DAY ==="
    echo "    (two BookSets over one feed; expect ~4.5 min and ~1.1 GB)"
    "$BIN/split_replay_gate" "$DAY" \
        --per-symbol-ref "$WORK/day-ref.csv" --per-symbol-split "$WORK/day-split.csv"
else
    echo
    echo "=== 5. the real day: SKIPPED (no feed given) ==="
    echo "    The generated feeds cannot show that the partition holds against real"
    echo "    NASDAQ references, because they do not contain any. Pass a day to close"
    echo "    that: ./scripts/split-replay-gate.sh data/raw/12302019.NASDAQ_ITCH50.gz"
fi

echo
echo "=== phase 12.1 gate: all detectors green ==="
