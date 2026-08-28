#!/usr/bin/env bash
#
# The packing-delay classification, tested rather than asserted.
#
# The design says t5a->t5b is the time a fill's ITCH 'E' spends waiting for the
# feed behind it to fill the datagram, so it is a function of --mtu and NOT a
# property of the machine. That is a DIRECTION, and a direction is
# machine-independent: it holds on this box even though none of the absolute
# numbers here may be published. Halving the MTU should roughly halve it.
#
# If the hop does not move with the knob, the classification is wrong and the
# hop is measuring something else.
set -u
cd /home/karee/itchbook
W=/home/karee/p128/mtu
mkdir -p "$W"

run_mtu() {   # $1 mtu, $2 tcp, $3 udp
    rm -f "$W/$1-ex.log" "$W/$1-st.log"
    build/exchange --feed data/raw/bench.gz --symbol TEST \
        --tcp-port "$2" --udp-port "$3" --limit 400000 --multiplier 0 \
        --wait-for-client --client-timeout-s 60 --cpu 2 --mtu "$1" \
        --trace-out "$W/$1-ex.trace" --json "$W/$1-ex.json" > "$W/$1-ex.log" 2>&1 &
    local ex=$!
    local i
    for i in $(seq 1 400); do
        grep -q "READY exchange" "$W/$1-ex.log" 2>/dev/null && break
        kill -0 $ex 2>/dev/null || break
        sleep 0.05
    done
    build/strategy --symbol TEST --tcp-port "$2" --udp-port "$3" \
        --quote-every 200 --max-orders 800 --quote-shares 100 \
        --quote-offset-ticks 2 --cpu 6 \
        --trace-out "$W/$1-st.trace" --json "$W/$1-st.json" > "$W/$1-st.log" 2>&1 &
    local st=$!
    wait $ex; wait $st
    echo "=== --mtu $1 ==="
    grep -E "mold packets|ITCH published" "$W/$1-ex.log"
    python3 scripts/phase12-8-report.py \
        --strategy-trace "$W/$1-st.trace" --exchange-trace "$W/$1-ex.trace" \
        --strategy-json "$W/$1-st.json" --exchange-json "$W/$1-ex.json" \
        --offset-bound-ns 52 2>&1 | grep -E "PACKING|joined on \(ref"
}

run_mtu 1400 28801 28802
run_mtu 700  28811 28812
run_mtu 350  28821 28822
