#!/usr/bin/env bash
#
# pinned-run.sh — take the phase 10 numbers on a machine where they mean
# something.
#
# Everything in docs/phase10-results.md today was measured in a two-core
# container with nothing pinned, on hardware where tools/tsc_offset reports the
# cross-core offset UNMEASURABLE. The tools say so themselves on every run
# ("NO RATE QUALIFIED", "UNPINNED"), and that is the honest state of the phase:
# the machinery is built, tested and gated, and the headline number does not
# exist yet.
#
# This is what takes it. Run it on a Linux box with at least four cores.
#
# THE CLOCK IS SETTLED FIRST, AND IT CAN VETO THE RUN. The wire-to-book sample
# is stamped on the receiver's core and closed on the book's, so it is a
# subtraction between two clocks. If they are not the same clock, or the offset
# between them is not small compared with the latency, the number is an artefact
# of the hardware rather than a property of the pipeline. So tsc_offset runs
# before the sweep and its answer gates it -- which is the reverse of the usual
# temptation, where the clock check is a footnote under a table already written.
#
# WHAT PINNING BUYS, AND WHY ALL THREE. Phase 4 measured 19.3% run-to-run
# variance on a SINGLE-threaded benchmark without pinning. This has three
# threads across two processes: a receiver, a book, and a load generator whose
# own lateness decides whether a run counts. Pin two and let the third float and
# the output says "pinned" while the floating one still owns the tail.
#
# Usage:
#   scripts/pinned-run.sh [--cores "2 3 4"] [--messages N] [--force-clock]
#
set -uo pipefail
cd "$(dirname "$0")/.."

CORES="${CORES:-}"
MESSAGES=4000000
FORCE_CLOCK=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cores)       CORES="$2"; shift 2 ;;
        --messages)    MESSAGES="$2"; shift 2 ;;
        --force-clock) FORCE_CLOCK=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This needs Linux. macOS has no thread-affinity API, which is why"
    echo "tsc_offset reports UNMEASURABLE there and why none of the numbers"
    echo "taken on a Mac can be quoted." >&2
    exit 2
fi

NPROC=$(nproc)
if [[ -z "$CORES" ]]; then
    if (( NPROC < 4 )); then
        echo "Only $NPROC cores. Three of them would be pinned and the OS would"
        echo "have what is left, which is the situation this script exists to"
        echo "escape. Use a bigger machine, or pass --cores to insist." >&2
        exit 2
    fi
    # The last three, on the assumption that core 0 carries interrupts and the
    # OS. On a machine with isolcpus set, pass those cores explicitly instead.
    CORES="$((NPROC - 3)) $((NPROC - 2)) $((NPROC - 1))"
fi
read -r CPU_RECV CPU_BOOK CPU_SEND <<<"$CORES"
echo "cores: receiver=$CPU_RECV book=$CPU_BOOK sender=$CPU_SEND (of $NPROC)"

# ---- the environment, reported rather than assumed --------------------------
#
# Each of these changes the numbers, and each is invisible in the output unless
# something prints it. A results file that does not say the governor was
# "powersave" is a results file with an unexplained 30% in it.
echo
echo "--- environment ---"
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
echo "  scaling governor        $GOV"
if [[ "$GOV" != "performance" && "$GOV" != "unknown" ]]; then
    echo "    ...frequency scaling will move these numbers between runs. For a"
    echo "    quotable figure: sudo cpupower frequency-set -g performance"
fi
if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    echo "  turbo disabled          $(cat /sys/devices/system/cpu/intel_pstate/no_turbo)  (1 = disabled, steadier)"
fi
ISOL=$(cat /sys/devices/system/cpu/isolated 2>/dev/null); echo "  isolated cores          ${ISOL:-none}"
RMEM=$(sysctl -n net.core.rmem_max 2>/dev/null || echo 0)
echo "  net.core.rmem_max       $RMEM"
if (( RMEM < 16777216 )); then
    echo "    ...below 16 MB, so SO_RCVBUF will be capped SILENTLY and the"
    echo "    kernel will drop datagrams the ring never sees. wire_to_book reads"
    echo "    the granted value back and reports it, so this will show up -- but"
    echo "    fix it first:  sudo sysctl -w net.core.rmem_max=67108864"
fi
echo "  clocksource             $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)"

# ---- build, optimised, no sanitizers ----------------------------------------
echo
echo "--- build (Release; a sanitised build measures the sanitiser) ---"
cmake -S . -B build-pinned -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-pinned -j >/dev/null
echo "  ok"

mkdir -p validation out

# ---- 1. the clock, which can veto everything after it -----------------------
echo
echo "--- 1. cross-core clock offset ---"
./build-pinned/tsc_offset --samples 200000 --cpu-a "$CPU_RECV" --cpu-b "$CPU_BOOK" \
    --json validation/tsc-offset.json
CLOCK_RC=$?
python3 - "$FORCE_CLOCK" <<'CLOCKCHECK'
import json, sys
force = sys.argv[1] == "1"
d = json.load(open("validation/tsc-offset.json"))
res = d["resolution_ns"]
print(f"  source      {d['timestamp_source']}")
print(f"  pinned      {d['pinned']}")
print(f"  resolution  {res:.0f} ns")
print(f"  resolvable  {d['resolvable']}")

# THREE OUTCOMES, NOT TWO, and the middle one is the usual one.
#
# The first version of this gate vetoed whenever `resolvable` was false, which
# reads as "the offset is unknown". That is not what the tool means. A ping-pong
# estimate is bounded by half the fastest round trip, so on healthy hardware the
# estimate comes back SMALLER than the method can resolve -- and tsc_offset says
# exactly that: "what is bounded is the offset, not measured". An offset bounded
# under 85 ns against a wire-to-book p50 in the microseconds is a rounding error
# you quote beside the number, which is what the tool's own verdict instructs.
# Vetoing there would have blocked the run on the first machine good enough for
# it, which is how this was found.
#
# What actually disqualifies a machine is a bound COMPARABLE with the thing
# being measured, or a failure to pin at all.
BOUND_NS = 1000.0

if not d["pinned"]:
    print()
    print("  VETO: the threads could not be pinned, so the two stamps did not come")
    print("  from the two cores this claims to compare.")
    sys.exit(1)

if d["resolvable"]:
    print()
    print("  Offset measured. Record it beside the latency figures.")
elif res <= BOUND_NS:
    print()
    print(f"  Offset BOUNDED under {res:.0f} ns rather than measured, which is what")
    print("  healthy hardware gives: the estimate is smaller than the method can")
    print("  resolve. That is small against a wire-to-book latency in the")
    print("  microseconds, so the sweep proceeds and the bound is reported")
    print("  beside the numbers rather than hidden.")
else:
    print()
    print(f"  VETO: the offset is only bounded under {res:.0f} ns, the same order as")
    print("  the latency being measured. Subtracting these two clocks would be")
    print("  arithmetic, not a latency.")
    print()
    print("  Two honest ways forward:")
    print("    - use CLOCK_MONOTONIC_RAW and say so (docs/phase10-methodology.md")
    print("      section 2), or")
    print("    - re-run with --force-clock, which proceeds and records the caveat.")
    if not force:
        sys.exit(1)
    print("  --force-clock given: proceeding with the caveat recorded.")
CLOCKCHECK
if [[ $? -ne 0 ]]; then exit 1; fi

# ---- 2. the sweep, pinned ---------------------------------------------------
#
# Five repeats rather than the two a container can afford: the noise is
# one-sided, best-of-N is what separates the pipeline from the machine's other
# tenants, and on a quiet pinned box five runs is a couple of minutes.
echo
echo "--- 2. rate-latency sweep (this is the long one) ---"
# REAL-TIME PRIORITY FOR THE LOAD GENERATOR, because pinning was not the
# obstacle. The first pinned run measured the sender's own pacing at a p50
# lateness of 35 ns and a p99.9 of 142 us: the loop is accurate to tens of
# nanoseconds when it runs, and its entire error budget goes to a tail where it
# is not running. A 1.09 ms maximum is a scheduler quantum. Spinning cannot help
# a thread that is off the CPU, and the observed tail is already inside the
# existing 500 us spin margin, so a bigger margin cannot either.
#
# SCHED_FIFO is the standard remedy and had never been asked for. It needs
# CAP_SYS_NICE; setcap grants it once without running the whole sweep as root.
# If neither works the sweep still runs and mold_replay_udp reports the denial
# per run, so a sweep that failed to get priority cannot be mistaken for one
# that had it.
if command -v setcap >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
    sudo -n setcap cap_sys_nice=ep build-pinned/mold_replay_udp 2>/dev/null \
        && echo "  CAP_SYS_NICE granted to the load generator" \
        || echo "  could not setcap (needs a sudo password); SCHED_FIFO may be denied"
fi

python3 bench/rate-sweep.py --build build-pinned --out validation/rate-sweep.json \
    --messages "$MESSAGES" --repeats 5 \
    --multipliers 1,2,5,10,25,50,100,200,400 --extend 6 \
    --rt-priority 80 \
    --cpu-recv "$CPU_RECV" --cpu-book "$CPU_BOOK" --cpu-sender "$CPU_SEND"

# ---- 3. the reader-thread overlap, on real cores ----------------------------
echo
echo "--- 3. reader-thread overlap ---"
python3 bench/reader-overlap.py --build build-pinned \
    --out validation/reader-overlap.json --messages "$MESSAGES" --repeats 5

# ---- 4. regenerate every document from what just ran ------------------------
echo
echo "--- 4. documents and figures, regenerated from the artifacts ---"
python3 python/analysis/rate_latency.py validation/rate-sweep.json \
    --svg docs/figures/rate-latency.svg \
    --hist-svg docs/figures/wire-to-book-hist.svg
python3 scripts/phase10-report.py
python3 scripts/phase10-8-report.py

echo
echo "--- what to check before quoting any of it ---"
python3 - <<'PY'
import json
d = json.load(open("validation/rate-sweep.json"))
ok = True
if not d.get("pinned"):
    print("  NOT PINNED — wire_to_book reported the threads unpinned"); ok = False
if d.get("sender_qualified_rates", 0) == 0:
    print("  NO RATE QUALIFIED — the sender missed its schedule everywhere;"
          "\n    these numbers describe the load generator"); ok = False
elif d["sender_qualified_rates"] < d["rates_tried"]:
    print(f"  {d['rates_tried'] - d['sender_qualified_rates']} of {d['rates_tried']}"
          " rates had the sender late; those rows are not quotable")
s = d.get("max_sustainable")
if s and s.get("is_lower_bound"):
    print("  max sustainable rate is a LOWER BOUND — the ladder never found the"
          "\n    cliff. Raise --extend."); ok = False
if ok:
    print("  clean: pinned, the sender held its schedule, and the cliff was found.")
    print(f"  max sustainable {s['achieved_rate']:,.0f} msg/s achieved, "
          f"p50 {s['p50_ns']:,.0f} ns, p99.9 {s['p999_ns']:,.0f} ns")
PY
echo
echo "Then: git add validation docs && git commit"
