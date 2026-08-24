#!/usr/bin/env bash
#
# determinism-gate.sh — the pipeline may reorder time. It may never reorder
# effect.
#
# GATE A. Below the knee, with drops at zero, the book built by two threads over
# a socket must be BYTE IDENTICAL to the book book_replay builds synchronously
# from the same file. Not close. Identical.
#
# And identical across timing regimes, not once. Three ring sizes and two rates
# means six different patterns of how full the ring got, how often the consumer
# starved, and how many messages a publish batched -- six different orderings in
# time, one book. A single configuration matching proves the code ran; six
# matching proves the answer does not depend on the schedule.
#
# Both sides use the same writer (include/itchbook/book/report.hpp) and the same
# BookSet construction. A second copy of the formatting would have made this
# comparison worthless in both directions: failing on a trailing zero, or
# passing because both copies were wrong in the same way.
#
# GATE B. Above the knee, when the pipeline IS dropping, every message it
# applied must be one the sender sent, exactly once, in order.
#
# The obvious test -- replay what was applied and compare books -- cannot detect
# the failure that matters. If the ring ever handed the consumer a slot the
# producer had already overwritten, the recording contains the corrupted message
# and a synchronous replay of the recording reproduces the same wrong book, and
# the comparison passes. So the recording is checked against the SENDER's feed
# instead: the applied stream must be an exact in-order subsequence of what was
# sent. That catches duplication, corruption, and reordering, none of which a
# book-to-book diff can see.
#
# Usage: scripts/determinism-gate.sh [build-dir]
set -uo pipefail

BUILD="${1:-build}"
PORT=${PORT:-26550}
# For the gate's own self-test. TORTURE_EXTRA passes a flag to the tool on the
# torture leg only; TORTURE_ONLY skips gate A, which the self-test does not need
# and which costs six runs. Neither is set by any normal invocation.
TORTURE_EXTRA="${TORTURE_EXTRA:-}"
TORTURE_ONLY="${TORTURE_ONLY:-}"
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
            book_replay -j >/dev/null; then
        echo "error: $BUILD failed to rebuild; refusing to gate on a stale binary" >&2
        exit 2
    fi
fi

for t in wire_to_book mold_replay_udp mold_wrap book_replay; do
    if [[ ! -x "$BUILD/$t" ]]; then echo "missing $BUILD/$t" >&2; exit 2; fi
done

wait_bound() {
    if [[ ! -r /proc/net/udp ]]; then sleep 2; return 0; fi
    local hex; hex=$(printf '%04X' "$1")
    for _ in $(seq 1 100); do
        if grep -qi ":$hex " /proc/net/udp 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "    receiver never bound port $1" >&2
    return 1
}

echo "==> feed"
python3 scripts/make-synthetic-feed.py "$WORK/feed.gz" --messages 200000 --symbols 64
"$BUILD/mold_wrap" "$WORK/feed.gz" "$WORK/feed.pkt.gz" >/dev/null

echo "==> the synchronous book (book_replay --all-symbols)"
"$BUILD/book_replay" "$WORK/feed.gz" --all-symbols --per-symbol "$WORK/sync.csv" --quiet
echo "    $(wc -l < "$WORK/sync.csv") rows"

fail=0
clean_runs=0
skipped=0

# ---- Gate A -----------------------------------------------------------------
#
# A configuration that DROPS is above the knee on this machine, and a book built
# from a feed with holes in it is not supposed to match one built from the whole
# feed. Such a run is skipped rather than failed -- but it is counted and named,
# and the gate fails if too few configurations stayed below the knee. Silently
# skipping every run would let a machine that drops everything report a pass
# having compared nothing, which is the same shape of lie as a clean sheet by
# construction.
port=$PORT
if [[ -n "$TORTURE_ONLY" ]]; then
    echo "    (gate A skipped: TORTURE_ONLY)"
fi
for ring in 12 14 16; do
    [[ -n "$TORTURE_ONLY" ]] && break
    for rate in 30000 90000; do
        port=$((port + 1))
        "$BUILD/wire_to_book" --port "$port" --ring-log2 "$ring" --timeout-ms 4000 \
            --rcvbuf-mb 16 --expect-messages 400000 --per-symbol "$WORK/wire.csv" \
            --json "$WORK/wire.json" --quiet > "$WORK/wire.txt" 2>&1 &
        pid=$!
        if ! wait_bound "$port"; then kill $pid 2>/dev/null; wait $pid 2>/dev/null; fail=1; continue; fi
        "$BUILD/mold_replay_udp" "$WORK/feed.pkt.gz" --host 127.0.0.1 --port "$port" \
            --rate "$rate" --quiet >/dev/null 2>&1
        wait $pid
        rc=$?
        if [[ $rc -eq 3 ]]; then
            drops=$(python3 -c "import json;j=json.load(open('$WORK/wire.json'));print(j['ring_full_drops'],'ring-full,',j['kernel_drops'],'kernel')" 2>/dev/null || echo "?")
            echo "    ring 2^$ring @ ${rate} msg/s: SKIP — above the knee here ($drops)"
            skipped=$((skipped + 1))
            continue
        fi
        if [[ $rc -ne 0 ]]; then
            echo "    ring 2^$ring @ ${rate} msg/s: FAIL — wire_to_book exited $rc"
            cat "$WORK/wire.txt"
            fail=1
            continue
        fi
        if ! cmp -s "$WORK/sync.csv" "$WORK/wire.csv"; then
            echo "    ring 2^$ring @ ${rate} msg/s: FAIL — the pipeline's book differs"
            diff "$WORK/sync.csv" "$WORK/wire.csv" | head -10
            fail=1
            continue
        fi
        echo "    ring 2^$ring @ ${rate} msg/s: identical"
        clean_runs=$((clean_runs + 1))
    done
done

# Four of the six configurations must have stayed below the knee, and they must
# span more than one ring size -- otherwise "identical" could be one lucky
# schedule rather than a property that survives changing the schedule.
if [[ -z "$TORTURE_ONLY" ]]; then
echo "    $clean_runs below the knee, $skipped skipped"
if [[ $clean_runs -lt 4 ]]; then
    echo "    FAIL: only $clean_runs configurations ran clean; the gate compared too little"
    fail=1
fi
fi

# ---- Gate B -----------------------------------------------------------------
echo
echo "==> torture: overload, then check what was applied against what was sent"
port=$((port + 1))
"$BUILD/wire_to_book" --port "$port" --ring-log2 10 --timeout-ms 4000 \
    --rcvbuf-mb 16 --expect-messages 400000 --applied-out "$WORK/applied.gz" \
    --json "$WORK/torture.json" $TORTURE_EXTRA --quiet > "$WORK/torture.txt" 2>&1 &
pid=$!
if wait_bound "$port"; then
    "$BUILD/mold_replay_udp" "$WORK/feed.pkt.gz" --host 127.0.0.1 --port "$port" \
        --rate 3000000 --quiet >/dev/null 2>&1
    wait $pid
    rc=$?
    if [[ $rc -ne 3 ]]; then
        # Two very different things arrive here and the distinction is the point
        # of the exit codes: 1 is a broken identity -- the pipeline lost track of
        # its own loss -- and 0 means the load simply was not enough to provoke
        # any. Saying "the overload did not overload" for both sent the last
        # reader looking at the sender when the fault was in the receiver.
        if [[ $rc -eq 1 ]]; then
            echo "    FAIL: exit 1 — an identity broke; the pipeline lost track of its own loss"
        else
            echo "    FAIL: expected exit 3 (lossy), got $rc — the overload did not overload"
        fi
        cat "$WORK/torture.txt"
        fail=1
    else
        python3 - "$WORK/feed.gz" "$WORK/applied.gz" "$WORK/torture.json" <<'PY' || fail=1
import gzip, json, struct, sys

def messages(path):
    with gzip.open(path, "rb") as f:
        while True:
            h = f.read(2)
            if len(h) < 2: return
            n = struct.unpack(">H", h)[0]
            b = f.read(n)
            if len(b) < n: return
            yield b

sent = list(messages(sys.argv[1]))
got = list(messages(sys.argv[2]))
j = json.load(open(sys.argv[3]))

# In-order subsequence, two pointers. A duplicated message fails because the
# second copy has no unconsumed match ahead of it; a corrupted one fails because
# its bytes match nothing; a reordered one fails because the match is behind.
i = 0
for k, m in enumerate(got):
    while i < len(sent) and sent[i] != m:
        i += 1
    if i == len(sent):
        print(f"    FAIL: applied message {k} is not in the sent feed at or after "
              f"this point (duplicate, corrupt, or out of order)")
        sys.exit(1)
    i += 1

if not got:
    print("    FAIL: the overload applied nothing at all")
    sys.exit(1)

# ...and the arithmetic has to close. Everything sent is applied, unmodelled,
# or accounted as loss.
applied = j["messages_applied"]
if len(got) != j["messages_into_ring"]:
    print(f"    FAIL: recorded {len(got)} applied, tool counted "
          f"{j['messages_into_ring']} into the ring")
    sys.exit(1)
accounted = (j["messages_into_ring"] + j["oversize_messages"] + j["staging_overflow"]
             + j["messages_lost"] + j["lost_before_sequencer"] + j["lost_after_sequencer"])
if accounted != j["wire_messages"]:
    print(f"    FAIL: {j['wire_messages']} on the wire, {accounted} accounted for")
    sys.exit(1)

# EVERY HOLE WAS ANNOUNCED, checked here as well as inside the tool. The tool
# asserting its own identity is one grader; this is a second one reading the
# artifact, which is the only kind of agreement worth anything when the thing
# under test is whether a program noticed its own loss.
#
# Three declared gaps and nine thousand messages refused mid-block can arrive at
# the book as four markers, because markers coalesce -- so the sum is in
# MESSAGES, never in events.
missing = j["messages_lost"] + j["oversize_messages"] + j["staging_overflow"]
if j["messages_lost_seen_by_book"] != missing:
    print(f"    FAIL: {missing} messages went missing, the book was told about "
          f"{j['messages_lost_seen_by_book']}")
    sys.exit(1)
if j["gaps_lost_to_full_ring"] != 0:
    print(f"    FAIL: {j['gaps_lost_to_full_ring']} missing messages were never announced")
    sys.exit(1)

# ...and the no-op rule, which every damaging scenario in this repo has had
# since phase 7. A marker that never had to wait for a slot means the overload
# did not overload the thing this leg exists to overload, and the pass would be
# a pass by construction -- which is exactly how the unstageable marker survived
# four commits of green CI.
if j["gap_markers_to_book"] == 0 or j["gap_markers_deferred"] == 0:
    print(f"    FAIL: {j['gap_markers_to_book']} markers, "
          f"{j['gap_markers_deferred']} of them made to wait -- the ring never got "
          f"full enough to test the path this leg is for")
    sys.exit(1)

print(f"    {len(got)} applied, every one an in-order match against the sent feed")
print(f"    {j['messages_lost']} declared lost; all {j['wire_messages']} accounted for")
print(f"    {missing} missing, carried to the book by {j['gap_markers_to_book']} markers "
      f"({j['gap_markers_deferred']} made to wait for a slot); 0 unannounced")
PY
    fi
else
    kill $pid 2>/dev/null; wait $pid 2>/dev/null; fail=1
fi

echo
if [[ $fail -ne 0 ]]; then echo "determinism gate FAILED"; exit 1; fi
echo "determinism gate PASSED"
