#!/usr/bin/env bash
# One instrumented run, pinned to a non-sibling pair, with the raw traces out.
set -u
cd /home/karee/itchbook
W=/home/karee/p128
mkdir -p "$W"
rm -f "$W"/ex.log "$W"/st.log "$W"/ex.trace "$W"/st.trace "$W"/ex.json "$W"/st.json

BIN="${BIN:-build}"

"$BIN/exchange" --feed data/raw/bench.gz --symbol TEST \
    --tcp-port 28501 --udp-port 28502 --limit 400000 --multiplier 0 \
    --wait-for-client --client-timeout-s 60 --cpu 2 \
    --trace-out "$W/ex.trace" --json "$W/ex.json" > "$W/ex.log" 2>&1 &
EX=$!
for _ in $(seq 1 400); do
    grep -q "READY exchange" "$W/ex.log" 2>/dev/null && break
    kill -0 $EX 2>/dev/null || break
    sleep 0.05
done
"$BIN/strategy" --symbol TEST --tcp-port 28501 --udp-port 28502 \
    --quote-every 200 --max-orders 800 --quote-shares 100 \
    --quote-offset-ticks 2 --cpu 6 \
    --trace-out "$W/st.trace" --json "$W/st.json" > "$W/st.log" 2>&1 &
ST=$!
wait $EX; echo "exchange exit=$?"
wait $ST; echo "strategy exit=$?"

echo "--- exchange ---"
grep -E "pinned to cpu|chain A|t3'|t4 stamps|dropped" "$W/ex.log"
echo "--- strategy ---"
grep -E "pinned to cpu|chain A|stamps|migrations|dropped" "$W/st.log"
ls -l "$W"/*.trace
