#!/usr/bin/env bash
#
# wire-to-book-check.sh — prove the UDP pipeline loses nothing, and prove it
# notices when it does.
#
# Two runs, and the second is the one that matters.
#
#   1. A modest rate into a large ring. Everything sent must arrive: zero ring
#      drops, zero kernel drops, zero mid-block refusals, exit 0.
#   2. The same feed at fifteen times the rate into a ring sixty-four times
#      smaller. This MUST lose messages -- and must exit 3 saying so, with
#      every lost message accounted for by the tool's own identities.
#
# Run 2 exists because run 1 alone is not evidence. A receiver that counted
# nothing would pass run 1 perfectly. The claim being tested is not "no
# messages were lost", it is "no message is lost WITHOUT BEING COUNTED", and
# the only way to test that is to make it lose some on purpose. A gate whose
# failure case is never exercised is a gate nobody has checked opens.
#
# Uses a synthetic feed, not the NASDAQ sample: this has to run on a machine
# with no market data on it. See scripts/make-synthetic-feed.py for what that
# does and does not buy.
#
# Usage: scripts/wire-to-book-check.sh [build-dir]
set -uo pipefail

BUILD="${1:-build}"
PORT_OK=${PORT_OK:-26501}
PORT_LOSSY=${PORT_LOSSY:-26502}
# Rates are overridable because the clean run's gate includes KERNEL drops, and
# what a machine can absorb on loopback is a property of the machine. A shared
# CI runner gets a conservative rate; the point of the run there is that the
# accounting holds, not that the box is fast. Real numbers come from a pinned
# host and go in docs/, not from here.
CLEAN_RATE=${CLEAN_RATE:-60000}
CLEAN_RING=${CLEAN_RING:-16}
LOSSY_RATE=${LOSSY_RATE:-3000000}
LOSSY_RING=${LOSSY_RING:-10}
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for t in wire_to_book mold_replay_udp mold_wrap; do
    if [[ ! -x "$BUILD/$t" ]]; then
        echo "missing $BUILD/$t — build it first" >&2
        exit 2
    fi
done

echo "==> generating a synthetic feed"
python3 scripts/make-synthetic-feed.py "$WORK/syn.gz" --messages 200000 --symbols 64
"$BUILD/mold_wrap" "$WORK/syn.gz" "$WORK/syn.pkt.gz" >/dev/null

SENT=$(python3 - "$WORK/syn.gz" <<'PY'
import gzip, struct, sys
n = 0
with gzip.open(sys.argv[1], "rb") as f:
    while True:
        h = f.read(2)
        if len(h) < 2: break
        f.seek(struct.unpack(">H", h)[0], 1)
        n += 1
print(n)
PY
)
echo "    $SENT messages on the wire"

fail=0

# Wait for the receiver to have the port open. A fixed sleep races: the sender
# is fire-and-forget, so a datagram sent before bind() is gone with nothing to
# report it, and the run fails for a reason that has nothing to do with the
# code under test.
wait_bound() {
    if [[ ! -r /proc/net/udp ]]; then sleep 2; return 0; fi   # not Linux
    local port_hex
    port_hex=$(printf '%04X' "$1")
    for _ in $(seq 1 100); do
        if grep -qi ":$port_hex " /proc/net/udp 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "    receiver never bound port $1" >&2
    return 1
}

run_case() {
    local name="$1" port="$2" ringlog2="$3" rate="$4" want_exit="$5"
    echo
    echo "==> $name (ring 2^$ringlog2 slots, $rate msg/s)"
    "$BUILD/wire_to_book" --port "$port" --ring-log2 "$ringlog2" --timeout-ms 4000 \
        --rcvbuf-mb 16 --expect-messages 400000 --json "$WORK/$name.json" \
        > "$WORK/$name.txt" 2>&1 &
    local pid=$!
    if ! wait_bound "$port"; then kill $pid 2>/dev/null; wait $pid 2>/dev/null; return 1; fi
    "$BUILD/mold_replay_udp" "$WORK/syn.pkt.gz" --host 127.0.0.1 --port "$port" \
        --rate "$rate" --quiet >/dev/null 2>&1
    wait $pid
    local rc=$?
    sed -n '/packets received/,$p' "$WORK/$name.txt" | head -20

    if [[ "$rc" != "$want_exit" ]]; then
        echo "    FAIL: exit $rc, expected $want_exit"
        cat "$WORK/$name.txt"
        return 1
    fi
    echo "    exit $rc as expected"
    return 0
}

# ---- run 1: nothing may be lost ---------------------------------------------
run_case clean "$PORT_OK" "$CLEAN_RING" "$CLEAN_RATE" 0 || fail=1

if [[ $fail -eq 0 ]]; then
    python3 - "$WORK/clean.json" "$SENT" <<'PY' || fail=1
import json, sys
j = json.load(open(sys.argv[1]))
sent = int(sys.argv[2])
bad = []
if j["messages_into_ring"] != sent:
    bad.append(f"{j['messages_into_ring']} of {sent} messages reached the ring")
for k in ("ring_full_drops", "kernel_drops", "staging_overflow",
          "messages_lost", "lost_before_sequencer", "lost_after_sequencer",
          "malformed_packets", "oversize_messages"):
    if j[k]:
        bad.append(f"{k} = {j[k]}")
if bad:
    print("    FAIL: " + "; ".join(bad))
    sys.exit(1)
print(f"    all {sent} messages arrived, nothing dropped anywhere")
PY
fi

# ---- run 2: it must lose messages, and must account for every one -----------
run_case lossy "$PORT_LOSSY" "$LOSSY_RING" "$LOSSY_RATE" 3 || fail=1

if [[ $fail -eq 0 ]]; then
    python3 - "$WORK/lossy.json" <<'PY' || fail=1
import json, sys
j = json.load(open(sys.argv[1]))
if j["ring_full_drops"] == 0 and j["staging_overflow"] == 0:
    print("    FAIL: the overload run lost nothing — the drop path is untested")
    sys.exit(1)
wire = j["wire_messages"]
accounted = (j["messages_into_ring"] + j["oversize_messages"] + j["staging_overflow"]
             + j["messages_lost"] + j["lost_before_sequencer"] + j["lost_after_sequencer"])
if wire != accounted:
    print(f"    FAIL: {wire} on the wire, {accounted} accounted for")
    sys.exit(1)
print(f"    {j['ring_full_drops']} packets dropped; all {wire} messages accounted for")
PY
fi

echo
if [[ $fail -ne 0 ]]; then
    echo "wire-to-book check FAILED"
    exit 1
fi
echo "wire-to-book check PASSED"
