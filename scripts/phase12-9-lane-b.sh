#!/usr/bin/env bash
#
# phase12-9-lane-b.sh — the LIVE lane of the phase 12.9 A/B.
#
# WHY --multiplier 1 AND NOT 1000. tools/exchange.cpp paces the feed by
#
#     elapsed_replay = (wall_now - wall_start) * multiplier
#
# so at 1000x one wall microsecond is one feed millisecond. Every committed
# real-day closed-loop run used 1000x, and at that pacing the strategy's ~13 us
# of real order-entry latency is ~13 MILLISECONDS of feed time. Handing that to
# the backtester as its latency model would make lane A *less* realistic than
# its 250 us default, which defeats the reason 12.9 was sequenced after 12.8.
#
# At 1x, wall time IS feed time: the measured latency applies directly, and both
# lanes can be given the same number in the same units. The cost is that
# replaying N minutes of tape takes N minutes.
#
# WHAT THIS DOES NOT DO. It does not modify tools/strategy.cpp. That binary's
# behaviour is what 12.7 proved and 12.8 measured; changing it to match the
# backtester would invalidate both. Lane A moves to lane B instead, and the
# asymmetries that remain are the phase's output rather than a problem to hide.
set -uo pipefail

BIN="${BIN:-build}"
FEED="${FEED:-data/sliced/MSFT.gz}"
SYMBOL="${SYMBOL:-MSFT}"
LOCATE="${LOCATE:-5291}"
OUT="${OUT:-/tmp/phase12-9}"
TCP_PORT="${TCP_PORT:-27411}"
UDP_PORT="${UDP_PORT:-27412}"
# Feed messages to replay. At 1x this also sets the wall clock: the sliced MSFT
# day is ~1.22M messages over 6.5 hours, so ~150k messages is roughly 25-45
# minutes depending on where the density falls.
LIMIT="${LIMIT:-150000}"
MULTIPLIER="${MULTIPLIER:-1}"
QUOTE_EVERY="${QUOTE_EVERY:-200}"
QUOTE_SHARES="${QUOTE_SHARES:-100}"
OFFSET_TICKS="${OFFSET_TICKS:-2}"
MAX_ORDERS="${MAX_ORDERS:-2000}"

mkdir -p "$OUT"
for t in exchange strategy; do
    if [[ ! -x "$BIN/$t" ]]; then
        echo "error: $BIN/$t is missing; build it first" >&2
        exit 2
    fi
done
if [[ ! -f "$FEED" ]]; then
    echo "error: feed $FEED not found (it is licensed and not committed)" >&2
    exit 2
fi

EXLOG="$OUT/lane-b-exchange.log"
STLOG="$OUT/lane-b-strategy.log"
rm -f "$EXLOG" "$STLOG"

echo "lane B: $SYMBOL  limit=$LIMIT  multiplier=${MULTIPLIER}x  quote_every=$QUOTE_EVERY"
echo "  at ${MULTIPLIER}x the replay is paced to the tape; this takes real time."

"$BIN/exchange" --feed "$FEED" --symbol "$SYMBOL" --locate "$LOCATE" \
    --tcp-port "$TCP_PORT" --udp-port "$UDP_PORT" \
    --limit "$LIMIT" --multiplier "$MULTIPLIER" \
    --wait-for-client --client-timeout-s 120 \
    --trace-out "$OUT/lane-b-exchange.trace" \
    --json "$OUT/lane-b-exchange.json" > "$EXLOG" 2>&1 &
EX=$!

# Wait for READY rather than for the port: phase 10 measured a 91-102 ms window
# between a port appearing and the process actually being ready.
for _ in $(seq 1 600); do
    grep -q "READY exchange" "$EXLOG" 2>/dev/null && break
    kill -0 "$EX" 2>/dev/null || break
    sleep 0.05
done
if ! grep -q "READY exchange" "$EXLOG" 2>/dev/null; then
    echo "!! exchange never became ready" >&2
    sed -n '1,25p' "$EXLOG" >&2
    wait "$EX" 2>/dev/null
    exit 1
fi

"$BIN/strategy" --symbol "$SYMBOL" \
    --tcp-port "$TCP_PORT" --udp-port "$UDP_PORT" \
    --quote-every "$QUOTE_EVERY" --quote-shares "$QUOTE_SHARES" \
    --quote-offset-ticks "$OFFSET_TICKS" --max-orders "$MAX_ORDERS" \
    --trace-out "$OUT/lane-b-strategy.trace" \
    --json "$OUT/lane-b-strategy.json" > "$STLOG" 2>&1
ST_RC=$?

wait "$EX" 2>/dev/null
EX_RC=$?

echo "exchange exit $EX_RC   strategy exit $ST_RC"
tail -4 "$STLOG"
echo "artifacts in $OUT"
exit $(( ST_RC != 0 ? ST_RC : 0 ))
