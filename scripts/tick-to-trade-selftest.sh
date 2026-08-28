#!/usr/bin/env bash
#
# The 12.8 mutation arm: stamp t3 where it is convenient rather than where the
# bytes leave.
#
# An order is bytes in a queue until a write takes them. Stamping t3 right after
# send_unsequenced measures the encode and calls it the write -- a mistake that
# makes every number LOOK BETTER, which is the kind that survives review. The
# telescoping identity cannot see it: the hops still sum, because the time has
# simply moved out of t2->t3 and into the un-instrumented remainder.
#
# The coverage check is what sees it. The chain span (iter_start -> t3) as a
# fraction of the iteration must fall, because the write is now outside the
# chain.
set -u
cd /home/karee/itchbook
W=/home/karee/p128/mut
mkdir -p "$W"

cp tools/strategy.cpp "$W/strategy.bak"

# Move the t3 stamp: record it at enqueue time instead of at the write.
python3 - "$W" <<'PYEOF'
import io, sys
P = "tools/strategy.cpp"
s = io.open(P, encoding="utf-8").read()
old = """                    awaiting_t3.emplace_back(token_seq, out.produced);"""
new = """                    // MUTANT: stamp t3 here, where the bytes are still in a
                    // queue, instead of at the write that puts them on the wire.
                    // The order is NOT queued for the real stamp, so nothing
                    // overwrites this one.
                    c->t3 = bench::mono_ns();
                    c->have |= bench::kHaveT3;
                    ++stamps.t3;"""
assert s.count(old) == 1, "mutation anchor"
io.open(P, "w", encoding="utf-8").write(s.replace(old, new, 1))
print("  mutation applied")
PYEOF

cmake --build build --target strategy > /dev/null 2>&1
cp "$W/strategy.bak" tools/strategy.cpp

rm -f "$W"/ex.log "$W"/st.log "$W"/ex.trace "$W"/st.trace "$W"/ex.json "$W"/st.json
build/exchange --feed data/raw/bench.gz --symbol TEST \
    --tcp-port 28701 --udp-port 28702 --limit 400000 --multiplier 0 \
    --wait-for-client --client-timeout-s 60 --cpu 2 \
    --trace-out "$W/ex.trace" --json "$W/ex.json" > "$W/ex.log" 2>&1 &
EX=$!
for _ in $(seq 1 400); do
    grep -q "READY exchange" "$W/ex.log" 2>/dev/null && break
    kill -0 $EX 2>/dev/null || break
    sleep 0.05
done
build/strategy --symbol TEST --tcp-port 28701 --udp-port 28702 \
    --quote-every 200 --max-orders 800 --quote-shares 100 \
    --quote-offset-ticks 2 --cpu 6 \
    --trace-out "$W/st.trace" --json "$W/st.json" > "$W/st.log" 2>&1 &
ST=$!
wait $EX; wait $ST

echo "--- MUTANT report ---"
python3 scripts/phase12-8-report.py \
    --strategy-trace "$W/st.trace" --exchange-trace "$W/ex.trace" \
    --strategy-json "$W/st.json" --exchange-json "$W/ex.json" \
    --offset-bound-ns 52 2>&1 | grep -E "chain span|t2->t3|t3->t3"

# Put the honest binary back.
cmake --build build --target strategy > /dev/null 2>&1
echo "--- restored ---"
