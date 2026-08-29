#!/usr/bin/env bash
#
# phase12-9-lane-a.sh — the REPLAY lane of the phase 12.9 A/B, swept over latency.
#
# WHY A SWEEP AND NOT ONE NUMBER. The plan says to set this lane's latency model
# to the measured hop latencies from the live lane. That instruction does not
# survive contact with tools/exchange.cpp:560:
#
#     elapsed_replay = (wall_now - wall_start) * multiplier
#
# The live lane paces the tape by a multiplier, so its wall-clock latency is
# multiplied into feed time. At the 1000x every committed real-day run used, 13
# microseconds of real order entry is 13 MILLISECONDS of feed time; at the 0x
# used here (replay as fast as possible) the relationship is undefined
# altogether. There is no single number that is honestly "the measured latency"
# in the units this lane consumes.
#
# So instead of picking one and hiding the choice, the lane is run across a
# range that spans the plausible answers, and the live lane's fill count is
# placed against the resulting curve. That converts the mismatch from a silent
# confound into a measured sensitivity: if fills barely move across two orders
# of magnitude of latency, the disagreement is not the latency model.
#
#   0          the floor -- no latency at all, an upper bound on fills
#   13000      12.8's measured decision-to-accept using the transport LOWER
#              bound: t1'->t3 8,139 + t3->t3' 1,750 + t3'->t4 2,537
#   20000      the same using the transport UPPER bound (t3pre->t3' 9,574)
#   250000     LatencyModel's own default, which every phase-6 run used
#   1000000    a millisecond, for scale
set -uo pipefail

BIN="${BIN:-build}"
FEED="${FEED:-data/sliced/MSFT.gz}"
OUT="${OUT:-/tmp/phase12-9}"
LIMIT="${LIMIT:-150000}"
QUOTE_EVERY="${QUOTE_EVERY:-200}"
OFFSET_TICKS="${OFFSET_TICKS:-2}"
MAX_ORDERS="${MAX_ORDERS:-2000}"
SIZE="${SIZE:-100}"
LATENCIES="${LATENCIES:-0 13000 20000 250000 1000000}"

mkdir -p "$OUT"
if [[ ! -x "$BIN/queue_backtest" ]]; then
    echo "error: $BIN/queue_backtest missing; build it first" >&2
    exit 2
fi
if [[ ! -f "$FEED" ]]; then
    echo "error: feed $FEED not found (licensed, not committed)" >&2
    exit 2
fi

echo "lane A: parity-maker, limit=$LIMIT, quote_every=$QUOTE_EVERY, offset=$OFFSET_TICKS"
for L in $LATENCIES; do
    "$BIN/queue_backtest" "$FEED" \
        --strategy parity-maker --limit "$LIMIT" \
        --quote-every "$QUOTE_EVERY" --offset-ticks "$OFFSET_TICKS" \
        --max-orders "$MAX_ORDERS" --size "$SIZE" \
        --latency-ns "$L" \
        --json "$OUT/lane-a-lat$L.json" \
        --fills-out "$OUT/lane-a-lat$L.jsonl" \
        > "$OUT/lane-a-lat$L.txt" 2>&1
    rc=$?
    if (( rc != 0 )); then
        echo "  latency $L: FAILED (exit $rc)"
        tail -3 "$OUT/lane-a-lat$L.txt"
        exit 1
    fi
    echo "  latency $L ns -> $OUT/lane-a-lat$L.json"
done
echo "done; artifacts in $OUT"
