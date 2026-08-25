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
if [[ -f "$BUILD/CMakeCache.txt" ]]; then
    if ! cmake --build "$BUILD" --target wire_to_book mold_replay_udp mold_wrap \
            -j >/dev/null; then
        echo "error: $BUILD failed to rebuild; refusing to gate on a stale binary" >&2
        exit 2
    fi
fi

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
# ...AND THEN FOR THE CONSUMER, which is a later event than the port opening.
#
# Between bind() and the book thread existing, wire_to_book calibrates the TSC
# (a 50 ms sleep), allocates its ref table and reserves two sample vectors --
# while the receiver thread is already stamping arrivals into a ring nothing is
# draining. Starting the generator at bind() therefore charges that whole window
# to the first messages of the run. wire_to_book prints READY once the consumer
# exists; wait for that instead of guessing with a sleep.
# $1 is the file the receiver's output is redirected to, $2 its pid. BOTH are
# needed: polling the file alone cannot tell "still starting up" from "died
# during bind", so a port collision used to burn the full 30 s and then report
# "never reported READY" while the real error sat unread in the file.
wait_ready() {
    local out="$1" pid="$2"
    for _ in $(seq 1 300); do
        if grep -q '^READY' "$out" 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "    receiver exited before it was ready:" >&2
            cat "$out" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "    receiver never reported READY:" >&2
    cat "$out" >&2
    return 1
}

# want_exit is an ALTERNATION, "0" or "0|4". Exit 4 is UNVERIFIED -- ring and
# staging were clean but kernel drops are unreadable -- and on a platform with
# no /proc/net/udp that is the correct outcome of a clean run, not a failure.
# Insisting on a single literal made this whole script fail on macOS the moment
# kernel_drops() started returning its "cannot tell you" sentinel instead of 0.
run_case() {
    local name="$1" port="$2" ringlog2="$3" rate="$4" want_exit="$5" rcvbuf="${6:-16}"
    echo
    echo "==> $name (ring 2^$ringlog2 slots, $rate msg/s, rcvbuf ${rcvbuf} MB)"
    "$BUILD/wire_to_book" --port "$port" --ring-log2 "$ringlog2" --timeout-ms 4000 \
        --rcvbuf-mb "$rcvbuf" --expect-messages 400000 --json "$WORK/$name.json" \
        > "$WORK/$name.txt" 2>&1 &
    local pid=$!
    if ! wait_ready "$WORK/$name.txt" "$pid"; then kill $pid 2>/dev/null; wait $pid 2>/dev/null; return 1; fi
    "$BUILD/mold_replay_udp" "$WORK/syn.pkt.gz" --host 127.0.0.1 --port "$port" \
        --rate "$rate" --quiet >/dev/null 2>&1
    wait $pid
    local rc=$?
    sed -n '/packets received/,$p' "$WORK/$name.txt" | head -20

    if [[ ! "$rc" =~ ^(${want_exit})$ ]]; then
        echo "    FAIL: exit $rc, expected $want_exit"
        cat "$WORK/$name.txt"
        return 1
    fi
    echo "    exit $rc as expected"
    return 0
}

# ---- run 1: nothing may be lost ---------------------------------------------
run_case clean "$PORT_OK" "$CLEAN_RING" "$CLEAN_RATE" '0|4' || fail=1

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

# ---- run 3: the KERNEL drop path, which had no test at all ------------------
#
# THIS IS A NEGATIVE SELF-TEST, and it exists because the gate it tests was
# dead for the whole life of the tool. wire_to_book read /proc/net/udp AFTER
# close(fd); a UDP socket leaves that file the instant it closes, so the read
# found no row, and kernel_drops() answered a missing row with 0 rather than
# its "cannot tell you" sentinel. Every run ever taken therefore recorded
# kernel_drops = 0 as a CONSTANT: exit 4 was unreachable, the kernel half of
# the LOSSY gate was dead code, and a sweep could publish a max sustainable
# rate while the kernel quietly discarded datagrams upstream of the ring --
# which is the exact failure the header of wire_to_book.cpp says the gate is
# there to prevent.
#
# Runs 1 and 2 cannot catch that: run 1 asserts kernel_drops == 0, which a
# broken gate also reports, and run 2 overflows the RING, which is a different
# counter. So: give the socket the smallest receive buffer the kernel will
# grant, offer a rate no 4 KB buffer can hold, and require the number to be
# NON-ZERO. A gate with no test that can fail is not a gate.
#
# DECIDED BEFORE THE RUN, not from the artifact after it. The first version
# asked run_case for exit 3 and left a "SKIP: cannot read kernel drops" branch
# in the Python below -- which could never print, because on such a platform the
# run exits 4, run_case fails first, and the guard skips the Python entirely.
if [[ ! -r /proc/net/udp ]]; then
    echo
    echo "==> kerndrop: SKIPPED — this platform has no /proc/net/udp, so there is"
    echo "    no kernel drop counter for the gate to read, let alone to test."
else
    kd_fail=0
    run_case kerndrop "$((PORT_LOSSY + 1))" 18 "$LOSSY_RATE" 3 0 || kd_fail=1
    fail=$(( fail | kd_fail ))

# Guarded on ITS OWN status, not the cumulative one: an unrelated failure in run
# 1 or 2 used to hide the kernel assertion completely.
if [[ $kd_fail -eq 0 ]]; then
    python3 - "$WORK/kerndrop.json" <<'PY' || fail=1
import json, sys
j = json.load(open(sys.argv[1]))
if j["kernel_drops"] is None:
    print("    FAIL: /proc/net/udp is readable but the tool reported the count "
          "as unknown")
    sys.exit(1)
if j["kernel_drops"] == 0:
    print("    FAIL: a 4 KB receive buffer under full rate dropped nothing "
          "according to the counter.\n"
          "    That is the dead-gate signature: kernel_drops is being reported "
          "as a constant.")
    sys.exit(1)
if j["ring_full_drops"] != 0:
    print(f"    note: the ring also dropped {j['ring_full_drops']} — the "
          "kernel assertion still stands")
print(f"    {j['kernel_drops']:,} datagrams dropped by the kernel and counted; "
      "the gate can fail")
PY
fi
fi

echo
if [[ $fail -ne 0 ]]; then
    echo "wire-to-book check FAILED"
    exit 1
fi
echo "wire-to-book check PASSED"
