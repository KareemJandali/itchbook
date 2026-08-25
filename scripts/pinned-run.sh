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
#                         [--skip-build]
#
set -uo pipefail
cd "$(dirname "$0")/.."

CORES="${CORES:-}"
MESSAGES=4000000
FORCE_CLOCK=0
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cores)       CORES="$2"; shift 2 ;;
        --messages)    MESSAGES="$2"; shift 2 ;;
        --force-clock) FORCE_CLOCK=1; shift ;;
        --skip-build)  SKIP_BUILD=1; shift ;;
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

# ---- ...AND ARE THEY THREE CORES? -------------------------------------------
#
# Nothing here used to ask. The header above argues at length that pinning two
# of three threads is worse than pinning none, because the output says "pinned"
# while the floating thread owns the tail -- and then the script accepted any
# three CPU numbers, including two hyperthreads of one physical core, which has
# the same shape of problem: it reports three pinned threads while two of them
# share a core's execution resources.
#
# It is not hypothetical. On the host this was written for, `isolcpus=13,14,15`
# spans only cores 6 and 7, so NO three-way distinct-core assignment exists
# inside the isolated set, and a run pinned to those three put the book and the
# sender on the two threads of core 7. sysfs knows; the script did not ask.
#
# A WARNING RATHER THAN A REFUSAL, because on that same host the collision made
# no measurable difference -- the knee, the cliff and the max sustainable rate
# came out identical either way -- and a machine with fewer cores than threads
# may have no better option. The point is that the run says so.
core_of() {
    cat "/sys/devices/system/cpu/cpu$1/topology/core_id" 2>/dev/null || echo "?"
}
CORE_RECV=$(core_of "$CPU_RECV"); CORE_BOOK=$(core_of "$CPU_BOOK"); CORE_SEND=$(core_of "$CPU_SEND")
if [[ "$CORE_RECV" == "?" ]]; then
    echo "  physical cores          unknown (no topology in sysfs)"
else
    echo "  physical cores          receiver=$CORE_RECV book=$CORE_BOOK sender=$CORE_SEND"
    if [[ "$CORE_RECV" == "$CORE_BOOK" || "$CORE_RECV" == "$CORE_SEND" \
          || "$CORE_BOOK" == "$CORE_SEND" ]]; then
        echo "    ...TWO OF THESE SHARE A PHYSICAL CORE. They are SMT siblings, so"
        echo "    two of the three threads are competing for one core's execution"
        echo "    resources while the output below reports all three as pinned."
        echo "    One CPU per physical core, from sysfs on THIS machine:"
        # Over every CPU sysfs knows about, not over nproc: nproc reports the
        # calling shell's AFFINITY MASK, so on a box with isolcpus set it
        # silently excludes exactly the CPUs you are most likely to want. That
        # is the same trap as the default-core rule above.
        for d in /sys/devices/system/cpu/cpu[0-9]*; do
            c=${d##*/cpu}
            id=$(cat "$d/topology/core_id" 2>/dev/null) || continue
            [[ -n "$id" ]] && printf '%s %s %s\n' "$id" "$c" \
                "$(cat "$d/topology/thread_siblings_list" 2>/dev/null)"
        done | sort -n -u -k1,1 | while read -r id c sib; do
            echo "      core $id: use cpu$c   (siblings $sib)"
        done
    fi
fi

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

# RT BANDWIDTH THROTTLING, which this script used to walk straight into.
#
# Below, it asks for SCHED_FIFO for the load generator. Linux caps a real-time
# thread at sched_rt_runtime_us of every sched_rt_period_us and DEQUEUES it for
# the remainder -- 50 ms at the defaults. The sender spins whenever the gap to
# its next deadline is inside the spin margin, so at any rate that saturates a
# core for longer than a period, asking for FIFO buys a 50 ms hole rather than
# a shorter tail -- and `dmesg | grep 'RT throttling activated'` says so when it
# fires. The flag was manufacturing the largest number in the table it was added
# to improve.
#
# What IS recorded, under validation/sender-qualification/ (sender alone, this
# machine, throttling lifted with sched_rt_runtime_us=-1 so the class is the
# only variable): nort_c14.json at priority 0 gives p99.9 241,385 ns, rt_c14.json
# at priority 80 gives 115,103 ns. So FIFO is worth about 2x once the throttle
# is out of the way, and worth less than nothing while it is in.
RT_RUNTIME=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo unknown)
RT_PERIOD=$(cat /proc/sys/kernel/sched_rt_period_us 2>/dev/null || echo 1000000)
[[ "$RT_PERIOD" =~ ^[0-9]+$ ]] || RT_PERIOD=1000000
echo "  sched_rt_runtime_us     $RT_RUNTIME of $RT_PERIOD"
# FAILS CLOSED. Only a value that positively shows throttling is off earns
# SCHED_FIFO; anything unreadable or unparseable gets priority 0. The first
# version tested for a NUMBER and left RT_PRIORITY at 80 otherwise, so an
# unreadable sysctl produced exactly the configuration this block exists to
# avoid -- while the sibling probe eight lines above (net.core.rmem_max) fails
# closed by substituting 0.
RT_PRIORITY=0
if [[ "$RT_RUNTIME" == "-1" ]]; then
    RT_PRIORITY=80
elif [[ "$RT_RUNTIME" =~ ^[0-9]+$ ]] && (( RT_RUNTIME >= RT_PERIOD )); then
    # Throttling turned off without using -1, which is a normal way to do it.
    echo "    ...runtime is not less than the period, so RT bandwidth throttling"
    echo "    does not bite. Asking for real-time priority."
    RT_PRIORITY=80
elif [[ "$RT_RUNTIME" =~ ^[0-9]+$ ]]; then
    echo "    ...so a saturating SCHED_FIFO thread is PARKED for"
    echo "    $(( (RT_PERIOD - RT_RUNTIME) / 1000 )) ms of every $(( RT_PERIOD / 1000 )) ms."
    echo "    Running the sweep WITHOUT real-time priority, which is the lesser"
    echo "    of the two evils. To use it:"
    echo "      sudo sysctl -w kernel.sched_rt_runtime_us=-1"
    echo "    (and put it back afterwards -- an unthrottled runaway FIFO thread"
    echo "     can leave you no way onto the machine)"
else
    echo "    ...unreadable, so real-time priority is NOT requested. A sweep that"
    echo "    cannot check for the throttle must not walk into it."
fi
MEMLOCK=$(ulimit -l 2>/dev/null || echo unknown)
echo "  ulimit -l               $MEMLOCK"
if [[ "$MEMLOCK" != "unlimited" ]]; then
    echo "    ...mlockall will fail with ENOMEM once the preloaded feed exceeds"
    echo "    this, which at --messages 4000000 it does by three orders of"
    echo "    magnitude. The generator reports what it was GRANTED, so this is"
    echo "    visible rather than assumed -- but a major fault in the pacing"
    echo "    loop is a millisecond no priority prevents."
fi

# ---- build, optimised, no sanitizers ----------------------------------------
echo
echo "--- build (Release; a sanitised build measures the sanitiser) ---"
if (( SKIP_BUILD )); then
    # FOR THE MACHINE THIS SCRIPT EXISTS FOR, which by construction is not the
    # machine you develop on. A live USB has no compiler and may have no
    # network, and installing a toolchain is not always possible on a box
    # borrowed for twenty minutes. Statically linked binaries dropped into
    # build-pinned/ run there with nothing installed at all:
    #
    #   g++ -O2 -std=c++20 -static -I include -o build-pinned/<t> tools/<t>.cpp -lz -lpthread
    #
    # The binaries are still Release and still unsanitised, which is what the
    # heading above is actually about.
    for t in wire_to_book mold_replay_udp mold_wrap itch_census tsc_offset; do
        if [[ ! -x "build-pinned/$t" ]]; then
            echo "  --skip-build given but build-pinned/$t is missing." >&2
            exit 2
        fi
    done
    echo "  skipped; using the binaries already in build-pinned/"
else
    cmake -S . -B build-pinned -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build-pinned -j >/dev/null
    echo "  ok"
fi

mkdir -p validation out

# ---- 1. the clock, which can veto everything after it -----------------------
echo
echo "--- 1. cross-core clock offset ---"
./build-pinned/tsc_offset --samples 200000 --cpu-a "$CPU_RECV" --cpu-b "$CPU_BOOK" \
    --json validation/tsc-offset.json
CLOCK_RC=$?
# READ, not merely assigned. The heredoc below opens validation/tsc-offset.json
# unconditionally, so a tsc_offset that died would leave the PREVIOUS run's
# clock artifact in place and the gate would grade that instead -- in the one
# step whose whole purpose is to be able to veto everything after it.
if (( CLOCK_RC != 0 )); then
    echo "tsc_offset exited $CLOCK_RC; the clock gate would grade a stale artifact." >&2
    exit "$CLOCK_RC"
fi
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
    # CAP_IPC_LOCK TOO. cap_sys_nice alone grants the scheduling class and not
    # the locked address space, so mlockall stayed subject to RLIMIT_MEMLOCK --
    # 64 KB by default -- and failed silently on any feed worth measuring.
    sudo -n setcap cap_sys_nice,cap_ipc_lock=ep build-pinned/mold_replay_udp 2>/dev/null \
        && echo "  CAP_SYS_NICE + CAP_IPC_LOCK granted to the load generator" \
        || echo "  could not setcap (needs a sudo password); SCHED_FIFO and mlockall may be denied"
fi

python3 bench/rate-sweep.py --build build-pinned --out validation/rate-sweep.json \
    --messages "$MESSAGES" --repeats 5 \
    --multipliers 1,2,5,10,25,50,100,200,400 --extend 6 \
    --rt-priority "$RT_PRIORITY" \
    --cpu-recv "$CPU_RECV" --cpu-book "$CPU_BOOK" --cpu-sender "$CPU_SEND"
SWEEP_RC=$?
# 3 IS NOT FAILURE, it is the sweep reporting that nothing qualified -- and the
# artifacts and documents still get written, because deleting the evidence is
# not the same as declining to quote it. Any OTHER non-zero means the sweep did
# not finish, and steps 3 and 4 below regenerate docs/phase10-results.md and
# both figures FROM validation/rate-sweep.json -- which would then be the
# PREVIOUS run's file, graded by the closing gate as though it were this one's.
if (( SWEEP_RC != 0 && SWEEP_RC != 3 )); then
    echo
    echo "The sweep exited $SWEEP_RC without finishing. validation/rate-sweep.json is"
    echo "now either half-written or left over from an earlier run, and every"
    echo "document below is generated from it. Stopping rather than grading the"
    echo "wrong run." >&2
    exit "$SWEEP_RC"
fi

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
REPORT_RC=$?
if (( REPORT_RC != 0 && REPORT_RC != 3 )); then
    echo "phase10-report.py failed ($REPORT_RC)" >&2
    exit "$REPORT_RC"
fi
python3 scripts/phase10-8-report.py

echo
echo "--- what to check before quoting any of it ---"
python3 - <<'PY'
import json, sys
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
if s is None:
    # The lowest rung on the ladder already dropped, so there is no sustainable
    # rate to report. Without this the `ok` branch below subscripts None and the
    # gate's verdict becomes a TypeError traceback printed under the heading
    # "what to check before quoting any of it".
    print("  NO SUSTAINABLE RATE — the lowest rung on the ladder already"
          "\n    dropped, so the sweep never measured one"); ok = False
elif s.get("is_lower_bound"):
    print("  max sustainable rate is a LOWER BOUND — the ladder never found the"
          "\n    cliff. Raise --extend."); ok = False
if ok:
    print("  clean: pinned, the sender held its schedule, and the cliff was found.")
    print(f"  max sustainable {s['achieved_rate']:,.0f} msg/s achieved, "
          f"p50 {s['p50_ns']:,.0f} ns, p99.9 {s['p999_ns']:,.0f} ns")
else:
    # AND IT EXITS. `ok` was set and then never read: the script printed its own
    # refusal, returned 0, and ended by suggesting a commit.
    sys.exit(3)
PY
GATE_RC=$?
if (( GATE_RC != 0 )); then
    echo
    echo "This run is NOT QUOTABLE for the reasons above. The artifacts and the"
    echo "generated documents were written anyway -- deleting them would hide the"
    echo "evidence -- and docs/phase10-results.md carries the same reasons at the"
    echo "top of its generated block. Do not fill in the CV line from this run."
    exit $GATE_RC
fi
echo
echo "Then: git add validation docs && git commit"
