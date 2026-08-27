#!/usr/bin/env bash
#
# closed-loop-check.sh — phase 12.7's gate.
#
# Two OS processes, two real sockets. tools/exchange.cpp replays an ITCH feed
# through the phase-12.1 split replayer and the phase-12.5 gateway, publishing
# everything as MoldUDP64 over UDP and speaking OUCH over SoupBinTCP.
# tools/strategy.cpp receives that UDP, rebuilds the book from it, quotes over
# TCP, and reports the fills it can see ON THE FEED.
#
# WHAT IS BEING CLAIMED, AND WHAT WOULD MAKE THE CLAIM EMPTY.
#
# The claim is PUBLISH -> TRANSPORT -> CONSUME: an ITCH 'E' naming a reference
# the strategy owns left the exchange as a datagram, arrived on the strategy's
# socket, and was recognised against a book that already held that reference
# because an ITCH 'A' for it crossed the same socket earlier.
#
# Three ways that claim could be reported green while being false, and the arm
# that kills each:
#
#   * The strategy learns its fills from OUCH and calls them feed fills.
#     ARM 3 deletes every OUCH Executed inside the strategy, before it is
#     counted. Fills must survive.
#   * The strategy invents fills -- an off-by-one, a stale map, a counter that
#     increments on the wrong branch, or a reference number remembered without
#     the Add that carried it ever having been applied to a book.
#     ARM 2 has the exchange withhold exactly the ITCH executions that name a
#     strategy reference, and nothing else. Fills must go to zero WHILE the
#     exchange still reports that those fills happened. A detector that cannot
#     see its input must report nothing; if it still reports something, it was
#     never reading the input.
#   * The strategy sees a fill it does not yet know is its own, and drops it.
#     ARM 4 holds every acknowledgement for 250ms while the feed runs at full
#     speed, which is the ordinary condition of an ack arriving late. The fills
#     must still add up. This cost 202 shares on the real day before it was
#     found, and it was found by two numbers that had never been compared.
#   * Nothing traded at all, so every arm passes by having no work to do.
#     ARM 1 is a required green baseline. Zero maker fills there is a FAILURE,
#     not a quiet pass -- this repo has produced a vacuous gate four times, and
#     three of those were a detector that could not run being scored as a
#     detector that found nothing.
#
# Every arm also cross-checks the two processes against each other: the shares
# the exchange says its aggressor took from strategy orders must equal the
# shares the strategy counted off the tape.
#
# BE PRECISE ABOUT WHAT THAT PROVES. split.hpp:376 does
#
#     c_.strategy_shares_taken += take;
#     emit_exec(p, sref, take);
#
# -- the same `take`. So this is ONE QUANTITY MEASURED TWICE, at the source and
# again after a round trip, not two independent computations. What it
# establishes is that every share the exchange put on the tape survived
# encoding, MoldUDP64 framing, a real UDP socket, sequencing, parsing, ownership
# attribution and the in-book test, with none dropped and none double-counted.
# That is a transport-and-attribution result and it is what this gate is for. It
# is NOT evidence that the quantity is arithmetically right -- conserves_shares
# and agrees_with_book are what speak to that, and arm 1 checks them separately.
#
set -uo pipefail

BIN="${BIN:-build}"
WORK="${WORK:-$(mktemp -d)}"
FEED="${FEED:-}"
TCP_PORT="${TCP_PORT:-27401}"
UDP_PORT="${UDP_PORT:-27402}"
LIMIT="${LIMIT:-400000}"
SYMBOL="${SYMBOL:-TEST}"
FAILED=0

mkdir -p "$WORK"

for t in exchange strategy; do
    if [[ ! -x "$BIN/$t" ]]; then
        echo "error: $BIN/$t not built. cmake --build $BIN --target $t" >&2
        exit 2
    fi
done

# ---- the feed ----------------------------------------------------------------
#
# The bench feed is generated rather than assumed present: data/raw is licensed
# market data and is not in the tree, so on a CI runner it is absent. Phase
# 12.1's gate learned this the hard way -- it refused to run rather than run
# weaker, which was right, and then generated its own feed, which was better.
if [[ -z "$FEED" ]]; then
    FEED="data/raw/bench.gz"
    if [[ ! -f "$FEED" ]]; then
        FEED="$WORK/bench.gz"
        if [[ ! -f "$FEED" ]]; then
            echo "=== generating the bench feed ==="
            python3 python/make_bench_feed.py --seed 3 --messages 500000 "$FEED" >/dev/null || {
                echo "!! no feed and none could be generated. This gate cannot run," >&2
                echo "!! which is an ERROR, not a pass." >&2
                exit 3
            }
        fi
    fi
fi
echo "feed:   $FEED"
echo "work:   $WORK"

# ---- one run -----------------------------------------------------------------
#
# $1 arm name, $2 extra exchange args, $3 extra strategy args.
# Ports move per arm so a socket lingering in TIME_WAIT from the previous arm
# cannot make the next one's bind fail.
ARM_NO=0
run_arm() {
    local name="$1" ex_extra="$2" st_extra="$3"
    ARM_NO=$((ARM_NO + 1))
    local tp=$((TCP_PORT + ARM_NO * 2))
    local up=$((UDP_PORT + ARM_NO * 2))
    local exlog="$WORK/$name-exchange.log" stlog="$WORK/$name-strategy.log"
    EXJSON="$WORK/$name-exchange.json"
    STJSON="$WORK/$name-strategy.json"
    rm -f "$exlog" "$stlog" "$EXJSON" "$STJSON"

    "$BIN/exchange" --feed "$FEED" --symbol "$SYMBOL" \
        --tcp-port "$tp" --udp-port "$up" --limit "$LIMIT" --multiplier 0 \
        --wait-for-client --client-timeout-s 60 \
        $ex_extra --json "$EXJSON" > "$exlog" 2>&1 &
    local ex=$!

    # Wait for READY, not for the port. Phase 10 measured a 91-102ms window
    # between a port appearing and a process actually being ready, and paid for
    # it in multi-millisecond latency samples at every rate.
    local i
    for i in $(seq 1 400); do
        grep -q "READY exchange" "$exlog" 2>/dev/null && break
        kill -0 "$ex" 2>/dev/null || break
        sleep 0.05
    done
    if ! grep -q "READY exchange" "$exlog" 2>/dev/null; then
        echo "!! $name: exchange never became ready"; sed -n '1,20p' "$exlog"
        wait "$ex" 2>/dev/null; return 1
    fi

    "$BIN/strategy" --symbol "$SYMBOL" --tcp-port "$tp" --udp-port "$up" \
        --quote-every 200 --max-orders 800 --quote-shares 100 \
        --quote-offset-ticks 2 \
        $st_extra --json "$STJSON" > "$stlog" 2>&1 &
    local st=$!

    wait "$ex"; local exrc=$?
    wait "$st"; local strc=$?
    if (( exrc != 0 || strc != 0 )); then
        echo "!! $name: exchange exit=$exrc strategy exit=$strc"
        tail -5 "$exlog"; tail -5 "$stlog"
        return 1
    fi
    if [[ ! -s "$EXJSON" || ! -s "$STJSON" ]]; then
        echo "!! $name: a process produced no JSON"; return 1
    fi
    return 0
}

# Reads one integer field out of a flat JSON object. Booleans come back as
# 1/0 so every check below is the same shape.
jget() {
    python3 -c "
import json,sys
v = json.load(open(sys.argv[1])).get(sys.argv[2])
if isinstance(v, bool): v = int(v)
print(-1 if v is None else v)
" "$1" "$2"
}

check() {   # name, actual, op, expected
    local what="$1" got="$2" op="$3" want="$4"
    local ok=0
    case "$op" in
        eq) [[ "$got" -eq "$want" ]] && ok=1 ;;
        gt) [[ "$got" -gt "$want" ]] && ok=1 ;;
        ge) [[ "$got" -ge "$want" ]] && ok=1 ;;
    esac
    if (( ok )); then
        printf '   ok    %-46s %s %s\n' "$what" "$op" "$want"
    else
        printf '   FAIL  %-46s %s %s   (got %s)\n' "$what" "$op" "$want" "$got"
        FAILED=1
    fi
}

# ==============================================================================
echo
echo "=== ARM 1 — baseline: the loop closes ========================================"
echo "    Required green. Everything below it is vacuous without this."
if run_arm baseline "" ""; then
    EX1="$EXJSON"; ST1="$STJSON"
    taken=$(jget "$EX1" strategy_shares_taken)
    seen=$(jget "$ST1" maker_fill_shares)
    echo "    exchange: aggressor took $taken shares from strategy orders"
    echo "    strategy: counted $seen shares off the tape"
    check "strategy adds seen on the feed"       "$(jget "$ST1" own_adds_seen_on_feed)" gt 0
    check "MAKER FILLS observed on the feed"     "$(jget "$ST1" maker_fills_from_feed)" gt 0
    check "  resting in the strategy's OWN book" "$(jget "$ST1" maker_fills_in_my_book)" gt 0
    check "  every one of them was in the book"  "$(jget "$ST1" maker_fills_in_my_book)" eq "$(jget "$ST1" maker_fills_from_feed)"
    check "  and the shares match the exchange"  "$seen" eq "$taken"
    # The strategy quotes one side only, so every fill it can see is a buy.
    check "  position derived from those fills"  "$(jget "$ST1" position_from_feed)" eq "$seen"
    check "  which is not zero"                  "$taken" gt 0
    check "datagrams received"                   "$(jget "$ST1" datagrams)" gt 0
    check "sequencer gaps"                       "$(jget "$ST1" gaps)" eq 0
    check "messages lost"                        "$(jget "$ST1" messages_lost)" eq 0
    check "OUCH rejected"                        "$(jget "$ST1" ouch_rejected)" eq 0
    check "reference-partition violations"       "$(jget "$EX1" partition_violations)" eq 0
    check "conserves_shares"                     "$(jget "$EX1" conserves_shares)" eq 1
    check "agrees_with_book"                     "$(jget "$EX1" agrees_with_book)" eq 1
else
    echo "   FAIL  baseline did not complete"; FAILED=1
fi

# ==============================================================================
echo
echo "=== ARM 2 — negative: withhold the ITCH executions ==========================="
echo "    The exchange drops ITCH 'E' naming a strategy reference, and nothing"
echo "    else. Fills must vanish from the strategy WHILE the exchange still"
echo "    reports that they happened."
if run_arm suppressed "--suppress-strategy-exec-itch" ""; then
    EX2="$EXJSON"; ST2="$STJSON"
    check "exchange still suppressed something"  "$(jget "$EX2" itch_suppressed)" gt 0
    # ...and only that. Suppressing the whole tape would satisfy every other
    # assertion in this arm while proving nothing about strategy executions.
    check "  and suppressed almost nothing else"  "$(( $(jget "$EX2" itch_published) ))" gt "$(( $(jget "$EX2" itch_suppressed) * 1000 ))"
    check "  the tape arrived essentially intact"  "$(( $(jget "$ST2" feed_messages) * 10 ))" gt "$(( $(jget "$ST1" feed_messages) * 9 ))"
    check "  and fills really did happen"        "$(jget "$EX2" strategy_shares_taken)" gt 0
    check "  the strategy was still fed"         "$(jget "$ST2" datagrams)" gt 0
    check "  and still had orders on the book"   "$(jget "$ST2" own_adds_seen_on_feed)" gt 0
    check "MAKER FILLS from the feed"            "$(jget "$ST2" maker_fills_from_feed)" eq 0
    check "  resting in the strategy's OWN book" "$(jget "$ST2" maker_fills_in_my_book)" eq 0
    check "  and its feed-derived position"      "$(jget "$ST2" position_from_feed)" eq 0
    check "OUCH still reported them"             "$(jget "$ST2" ouch_executed_received)" gt 0
else
    echo "   FAIL  negative arm did not complete"; FAILED=1
fi

# ==============================================================================
echo
echo "=== ARM 3 — positive: delete OUCH Executed inside the strategy ==============="
echo "    Every OUCH Executed is discarded before it is counted. Fills must"
echo "    survive, because they never came from there."
if run_arm dropouch "" "--drop-ouch-executed"; then
    EX3="$EXJSON"; ST3="$STJSON"
    check "OUCH Executed seen by the strategy"   "$(jget "$ST3" ouch_executed_received)" eq 0
    check "MAKER FILLS from the feed"            "$(jget "$ST3" maker_fills_from_feed)" gt 0
    check "  resting in the strategy's OWN book" "$(jget "$ST3" maker_fills_in_my_book)" gt 0
    check "  every one of them was in the book"  "$(jget "$ST3" maker_fills_in_my_book)" eq "$(jget "$ST3" maker_fills_from_feed)"
    check "  shares match the exchange"          "$(jget "$ST3" maker_fill_shares)" eq "$(jget "$EX3" strategy_shares_taken)"
    check "  and are not zero"                   "$(jget "$ST3" maker_fill_shares)" gt 0
    check "  position derived from those fills"  "$(jget "$ST3" position_from_feed)" eq "$(jget "$ST3" maker_fill_shares)"
    check "sequencer gaps"                       "$(jget "$ST3" gaps)" eq 0
else
    echo "   FAIL  positive arm did not complete"; FAILED=1
fi

# ==============================================================================
echo
echo "=== ARM 4 — the acknowledgement arrives late ================================="
echo "    Every OUCH Accepted is held 250ms while the feed runs at full speed."
echo "    The race must actually happen, and must cost nothing."
if run_arm ackdelay "" "--ack-delay-ms 250"; then
    EX4="$EXJSON"; ST4="$STJSON"
    echo "    $(jget "$ST4" execs_before_ack) fills arrived before the ack that named them"
    # Without this the arm is vacuous: a delay that changed no ordering would
    # pass every check below by never exercising anything.
    check "the race actually happened"           "$(jget "$ST4" execs_before_ack)" gt 0
    check "  and so did the add-side race"       "$(jget "$ST4" adds_before_ack)" gt 0
    check "MAKER FILLS from the feed"            "$(jget "$ST4" maker_fills_from_feed)" gt 0
    check "  shares still match the exchange"    "$(jget "$ST4" maker_fill_shares)" eq "$(jget "$EX4" strategy_shares_taken)"
    check "  every one still in the book"        "$(jget "$ST4" maker_fills_in_my_book)" eq "$(jget "$ST4" maker_fills_from_feed)"
    check "  position still equals the shares"   "$(jget "$ST4" position_from_feed)" eq "$(jget "$ST4" maker_fill_shares)"
    check "sequencer gaps"                       "$(jget "$ST4" gaps)" eq 0
else
    echo "   FAIL  late-ack arm did not complete"; FAILED=1
fi

echo
if (( FAILED )); then
    echo "=== FAILED ==================================================================="
    echo "    Logs and JSON in $WORK"
    exit 1
fi
echo "=== PASSED ==================================================================="
echo "    A strategy order crossed TCP as OUCH, rested in the exchange's book,"
echo "    was published as ITCH over UDP, was executed by a historical order"
echo "    arriving behind it, and the execution was recognised by the strategy"
echo "    from the feed alone. Withholding the feed executions silences it;"
echo "    deleting the OUCH executions does not; and holding the acknowledgements"
echo "    back 250ms costs it nothing."
echo "    Artifacts in $WORK"
