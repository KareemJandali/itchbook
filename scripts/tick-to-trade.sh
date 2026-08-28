#!/usr/bin/env bash
#
# tick-to-trade.sh — phase 12.8's run harness, and the pre-flight that decides
# whether the run is worth taking at all.
#
# The expensive resource here is a bare-metal boot: a live session with no
# compiler, no network, and everything lost at reboot. Phase 10 spent one of
# those and then found four harness defects that had corrupted its numbers, so
# the order of operations below is the point of the file. Nothing is measured
# until the machine has been shown capable of being measured, and nothing is
# measured for sixty seconds until a short run has shown every hop takes samples.
#
# EXIT CODES, following scripts/pinned-run.sh:
#   0  ran, and the numbers may be quoted
#   3  ran correctly, and the numbers may NOT be quoted
#   1  broken
#   2  usage
#   4  a precondition was not met, so nothing ran
#
# 3 IS NOT FAILURE. The artifacts are still written, with the verdict and every
# input it was computed from inside the same object, because deleting the
# evidence is not the same as declining to quote it -- and because it is the
# only way to test this harness before the boot.
#
# THE DECISIVE GATE IS cpu_jitter, NOT THE CLOCK CHECKS. Every clock gate this
# repository owns passes on the WSL2 box: constant_tsc and nonstop_tsc,
# clocksource=tsc, and a cross-core offset bounded under 52 ns against a
# threshold of 1000. A full seven-hop table would be produced there with no gate
# having fired, and it would be wrong. cpu_jitter is what separates the two
# machines -- ~3,000 gaps per second over 10 us here against 30.9 on bare metal
# -- so it is the check that stops the run, and the clock checks are necessary
# rather than sufficient.
#
# EVERY TOOL'S EXIT CODE IS CHECKED BEFORE ITS FILE IS READ. pinned-run.sh
# carries that guard twice, with the comment "the clock gate would grade a stale
# artifact", because it happened.
#
set -uo pipefail

BIN="${BIN:-build}"
OUT="${OUT:-validation}"
WORK="${WORK:-$(mktemp -d)}"
FEED="${FEED:-}"
SYMBOL="${SYMBOL:-TEST}"
LOCATE="${LOCATE:-1}"
CPU_A="${CPU_A:-}"          # exchange
CPU_B="${CPU_B:-}"          # strategy
REPEATS="${REPEATS:-10}"
MULTIPLIER="${MULTIPLIER:-1000}"
JITTER_SECONDS="${JITTER_SECONDS:-20}"
TCP_PORT="${TCP_PORT:-29001}"
UDP_PORT="${UDP_PORT:-29002}"
DRY_LIMIT="${DRY_LIMIT:-120000}"
LIMIT="${LIMIT:-0}"
TAG="${TAG:-$(hostname)}"

mkdir -p "$WORK" "$OUT"
NOTQ=()      # reasons the numbers may not be quoted
say() { printf '%s\n' "$*"; }
step() { printf '\n=== %s\n' "$*"; }

for t in exchange strategy tsc_offset cpu_jitter; do
    if [[ ! -x "$BIN/$t" ]]; then
        say "error: $BIN/$t is not built. On the live session there is no compiler:"
        say "       build every binary STATIC before the boot."
        exit 4
    fi
done
if [[ ! -x scripts/phase12-8-report.py ]]; then
    say "error: scripts/phase12-8-report.py missing"; exit 4
fi

# ---- 0. provenance, before anything can be blamed on the wrong tree ----------
step "0. provenance"
COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
DIRTY=false
if ! git diff --quiet 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
    DIRTY=true
    NOTQ+=("the working tree is dirty: the binaries may not be the commit")
fi
say "  commit $COMMIT  dirty=$DIRTY"
say "  exchange sha256 $(sha256sum "$BIN/exchange" | cut -c1-16)"
say "  strategy sha256 $(sha256sum "$BIN/strategy" | cut -c1-16)"

# ---- 1. topology, read on the day -------------------------------------------
#
# nproc reports the CALLING SHELL'S AFFINITY MASK, not the machine, so it is
# never used to choose a CPU. Adjacent CPU numbers are SMT siblings on this
# hardware and the 0-7/8-15 split people assume is not how it is laid out.
step "1. topology"
sib() { cat "/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list" 2>/dev/null; }
# Is cpu $2 inside a sysfs cpu list like "6-7" or "2,5,9-11"? Empty list = no.
in_list() {
    [[ -z "$1" ]] && return 1
    python3 - "$1" "$2" <<'PYIL'
import sys
lst, cpu = sys.argv[1], int(sys.argv[2])
for part in lst.split(','):
    if '-' in part:
        lo, hi = part.split('-')
        if int(lo) <= cpu <= int(hi): sys.exit(0)
    elif part and int(part) == cpu: sys.exit(0)
sys.exit(1)
PYIL
}
pkg() { cat "/sys/devices/system/cpu/cpu$1/topology/physical_package_id" 2>/dev/null; }

if [[ -z "$CPU_A" || -z "$CPU_B" ]]; then
    say "error: set CPU_A (exchange) and CPU_B (strategy) explicitly."
    say "       Do not inherit phase 10's numbering; re-read the topology on the day."
    say "       Candidates on this machine:"
    ISO="$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
    [[ -n "$ISO" ]] && say "       (isolated cpus: $ISO -- prefer a non-sibling pair from these)"
    for c in $(ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | sed 's#.*/cpu##' | sort -n); do
        s="$(sib "$c")"; p="$(pkg "$c")"
        mark=""
        in_list "$ISO" "$c" && mark=" ISOLATED"
        printf '         cpu%-3s siblings=%-8s package=%s%s\n' "$c" "${s:-absent}" "${p:-absent}" "$mark"
    done
    exit 4
fi

SIB_A="$(sib "$CPU_A")"; SIB_B="$(sib "$CPU_B")"
PKG_A="$(pkg "$CPU_A")"; PKG_B="$(pkg "$CPU_B")"
say "  exchange cpu $CPU_A  siblings=${SIB_A:-absent}  package=${PKG_A:-absent}"
say "  strategy cpu $CPU_B  siblings=${SIB_B:-absent}  package=${PKG_B:-absent}"

if [[ "$CPU_A" == "$CPU_B" ]]; then
    say "REFUSED: both processes on one CPU. The TCP hop would measure a context"
    say "         switch of a few microseconds and be publishable as a gateway latency."
    exit 4
fi
# Absent is not a pass: a topology that cannot be read cannot be shown safe.
if [[ -z "$SIB_A" || -z "$SIB_B" ]]; then
    say "REFUSED: thread_siblings_list is unreadable, so the two CPUs cannot be"
    say "         shown NOT to be SMT siblings. Absent is not a pass."
    exit 4
fi
if python3 - "$SIB_A" "$CPU_B" <<'PY'
import sys
def contains(lst, cpu):
    for part in lst.split(','):
        if '-' in part:
            lo, hi = part.split('-');
            if int(lo) <= cpu <= int(hi): return True
        elif part and int(part) == cpu: return True
    return False
sys.exit(0 if contains(sys.argv[1], int(sys.argv[2])) else 1)
PY
then
    say "REFUSED: cpu $CPU_A and cpu $CPU_B are SMT siblings. They share execution"
    say "         ports AND one TSC, so tsc_offset would report near zero while every"
    say "         hop was inflated by contention -- a check that cannot fail."
    exit 4
fi
if [[ -z "$PKG_A" || -z "$PKG_B" ]]; then
    say "REFUSED: physical_package_id is unreadable. Cross-socket TSC synchronisation"
    say "         is a firmware property; absent is not a pass."
    exit 4
fi
if [[ "$PKG_A" != "$PKG_B" ]]; then
    say "REFUSED: cpu $CPU_A and cpu $CPU_B are on different physical packages. A box"
    say "         can keep clocksource=tsc while the packages differ by far more than"
    say "         the offset bound, silently rewriting both cross-process hops."
    exit 4
fi
say "  pair accepted: distinct physical cores, same package"
ISOLATED="$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
if [[ -n "$ISOLATED" ]]; then
    say "  isolated cpus: $ISOLATED"
    if ! in_list "$ISOLATED" "$CPU_A" || ! in_list "$ISOLATED" "$CPU_B"; then
        NOTQ+=("this machine isolates cpus $ISOLATED and the run used $CPU_A/$CPU_B: \
it measured contention it did not have to have")
    else
        say "  both chosen cpus are isolated"
    fi
fi

# ---- 2. the environment, three-state ------------------------------------------
step "2. environment"
probe() {   # name path
    if [[ -r "$2" ]]; then printf '  %-22s %s\n' "$1" "$(head -c 200 "$2" | head -1)"
    elif [[ -e "$2" ]]; then printf '  %-22s UNREADABLE\n' "$1"
    else printf '  %-22s ABSENT\n' "$1"; fi
}
CLOCKSRC_PATH=/sys/devices/system/clocksource/clocksource0/current_clocksource
probe clocksource "$CLOCKSRC_PATH"
probe governor "/sys/devices/system/cpu/cpu$CPU_A/cpufreq/scaling_governor"
probe no_turbo /sys/devices/system/cpu/intel_pstate/no_turbo
probe isolated /sys/devices/system/cpu/isolated
probe kernel /proc/version
probe rmem_max /proc/sys/net/core/rmem_max

CLOCKSRC_BEFORE=""
[[ -r "$CLOCKSRC_PATH" ]] && CLOCKSRC_BEFORE="$(cat "$CLOCKSRC_PATH")"
if [[ "$CLOCKSRC_BEFORE" != "tsc" ]]; then
    NOTQ+=("clocksource is '${CLOCKSRC_BEFORE:-absent}', not tsc")
fi
if grep -qiE "microsoft-standard-WSL|hypervisor" /proc/version /proc/cpuinfo 2>/dev/null; then
    NOTQ+=("running under a hypervisor")
fi

# ---- 3. cpu_jitter, THE DECISIVE GATE ----------------------------------------
step "3. cpu_jitter on exactly the two CPUs that will be used"
JIT="$WORK/jitter-pre.json"
"$BIN/cpu_jitter" --cpus "$CPU_A,$CPU_B" --seconds "$JITTER_SECONDS" --json "$JIT" \
    > "$WORK/jitter-pre.txt" 2>&1
JIT_RC=$?
if (( JIT_RC != 0 )); then
    say "cpu_jitter exited $JIT_RC; refusing to grade a stale or missing artifact."
    tail -5 "$WORK/jitter-pre.txt"
    exit 1
fi
if [[ ! -s "$JIT" ]]; then say "cpu_jitter wrote no JSON"; exit 1; fi
tail -3 "$WORK/jitter-pre.txt"

JIT_VERDICT=$(python3 - "$JIT" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
bad = []
for c in d.get("cpus", []):
    over = c.get("gaps_over", {}).get("100us")
    if over is None:
        bad.append("cpu %s has no gaps_over.100us field" % c.get("cpu"))
    elif over > 0:
        bad.append("cpu %s was off-CPU over 100us %d times (worst %s ns)"
                   % (c.get("cpu"), over, c.get("gap_ns", {}).get("max")))
print("|".join(bad))
PY
)
if [[ -n "$JIT_VERDICT" ]]; then
    IFS='|' read -ra parts <<< "$JIT_VERDICT"
    for p in "${parts[@]}"; do NOTQ+=("cpu_jitter: $p"); done
    say "  NOT QUOTABLE: $JIT_VERDICT"
else
    say "  both CPUs held their core: no gap over 100 us"
fi

# ---- 4. tsc_offset, both directions, exit code checked first -----------------
step "4. tsc_offset on the exact pinned pair"
OFF="$WORK/tsc-offset-pre.json"
"$BIN/tsc_offset" --samples 200000 --cpu-a "$CPU_A" --cpu-b "$CPU_B" --json "$OFF" \
    > "$WORK/tsc-offset-pre.txt" 2>&1
OFF_RC=$?
if (( OFF_RC != 0 )); then
    say "tsc_offset exited $OFF_RC; the clock gate would grade a stale artifact."
    tail -5 "$WORK/tsc-offset-pre.txt"
    exit 1
fi
if [[ ! -s "$OFF" ]]; then say "tsc_offset wrote no JSON"; exit 1; fi
grep -E "VERDICT|resolution|largest" "$WORK/tsc-offset-pre.txt" | head -4

BOUND_NS=$(python3 - "$OFF" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# The bound is the METHOD'S RESOLUTION when the estimate is smaller than it --
# what is bounded is the offset, not measured. Take the larger of the two so the
# figure quoted beside a hop is never optimistic.
res = d.get("resolution_ns") or d.get("uncertainty_ns") or 0.0
est = abs(d.get("largest_abs_offset_ns") or 0.0)
print("%.3f" % max(res, est))
PY
)
say "  offset bound for the pair: $BOUND_NS ns"

# ---- 5. the feed ---------------------------------------------------------------
if [[ -z "$FEED" ]]; then
    FEED="data/raw/bench.gz"
    if [[ ! -f "$FEED" ]]; then
        FEED="$WORK/bench.gz"
        [[ -f "$FEED" ]] || python3 python/make_bench_feed.py --seed 3 --messages 500000 "$FEED" >/dev/null || {
            say "no feed and none could be generated"; exit 4; }
    fi
fi
say ""
say "feed:   $FEED   symbol=$SYMBOL locate=$LOCATE"

# ---- 6. the dry run, BEFORE spending the boot --------------------------------
#
# A hop with zero samples scored as a pass is the specific failure that already
# happened twice in 12.7. This is short on purpose: it exists to find that
# before sixty seconds a run have been spent, not to produce a number.
step "6. dry run: does every hop take samples?"
one_run() {   # $1 tag  $2 limit  $3 extra-exchange  $4 extra-strategy
    local tag="$1" lim="$2" exx="$3" stx="$4"
    local tp=$((TCP_PORT + RANDOM % 200 * 2))
    local up=$((tp + 1))
    rm -f "$WORK/$tag-ex.log" "$WORK/$tag-st.log"
    "$BIN/exchange" --feed "$FEED" --symbol "$SYMBOL" --locate "$LOCATE" \
        --tcp-port "$tp" --udp-port "$up" --limit "$lim" --multiplier "$MULTIPLIER" \
        --wait-for-client --client-timeout-s 60 --cpu "$CPU_A" \
        $exx --trace-out "$WORK/$tag-ex.trace" --json "$WORK/$tag-ex.json" \
        > "$WORK/$tag-ex.log" 2>&1 &
    local ex=$!
    local i
    for i in $(seq 1 600); do
        grep -q "READY exchange" "$WORK/$tag-ex.log" 2>/dev/null && break
        kill -0 $ex 2>/dev/null || break
        sleep 0.05
    done
    if ! grep -q "READY exchange" "$WORK/$tag-ex.log" 2>/dev/null; then
        say "  $tag: exchange never became ready"; tail -3 "$WORK/$tag-ex.log"
        wait $ex 2>/dev/null; return 1
    fi
    "$BIN/strategy" --symbol "$SYMBOL" --tcp-port "$tp" --udp-port "$up" \
        --quote-every 200 --max-orders 1200 --quote-shares 100 \
        --quote-offset-ticks 2 --cpu "$CPU_B" \
        $stx --trace-out "$WORK/$tag-st.trace" --json "$WORK/$tag-st.json" \
        > "$WORK/$tag-st.log" 2>&1 &
    local st=$!
    wait $ex; local exrc=$?
    wait $st; local strc=$?
    if (( exrc != 0 || strc != 0 )); then
        say "  $tag: exchange exit=$exrc strategy exit=$strc"
        tail -3 "$WORK/$tag-ex.log"; tail -3 "$WORK/$tag-st.log"
        return 1
    fi
    return 0
}

if ! one_run dry "$DRY_LIMIT" "" ""; then
    say "REFUSED: the dry run did not complete. Fix that before spending the boot."
    exit 1
fi
python3 scripts/phase12-8-report.py \
    --strategy-trace "$WORK/dry-st.trace" --exchange-trace "$WORK/dry-ex.trace" \
    --strategy-json "$WORK/dry-st.json" --exchange-json "$WORK/dry-ex.json" \
    --offset-bound-ns "$BOUND_NS" --jitter-json "$JIT" \
    > "$WORK/dry-report.txt" 2>&1
DRY_RC=$?
if (( DRY_RC == 1 )); then
    say "REFUSED: the dry run's report says the harness is BROKEN, not merely"
    say "         unquotable. Nothing below would be worth taking."
    grep -E "^  !" "$WORK/dry-report.txt"
    exit 1
fi
# Every hop must have taken samples, and there must be fills to report on.
if grep -qE "took ZERO samples" "$WORK/dry-report.txt"; then
    say "REFUSED: a hop took zero samples in the dry run."
    grep "ZERO samples" "$WORK/dry-report.txt"
    exit 1
fi
DRY_FILLS=$(grep -oE "joined on \(reference, fill ordinal\) *[0-9]+" "$WORK/dry-report.txt" | grep -oE "[0-9]+$" || echo 0)
say "  dry run: chain B joined $DRY_FILLS fills"
if [[ "${DRY_FILLS:-0}" -eq 0 ]]; then
    say "REFUSED: the dry run produced no maker fills, so chain B would be empty."
    say "         Check --locate: it is 1 for every generated feed and wrong for"
    say "         every real one (MSFT is 5291)."
    exit 1
fi
say "  every hop took samples"

# ---- 7. the measurement --------------------------------------------------------
step "7. $REPEATS repeats at --multiplier $MULTIPLIER"
OK=0
for r in $(seq 1 "$REPEATS"); do
    if one_run "run$r" "$LIMIT" "" ""; then
        python3 scripts/phase12-8-report.py \
            --strategy-trace "$WORK/run$r-st.trace" --exchange-trace "$WORK/run$r-ex.trace" \
            --strategy-json "$WORK/run$r-st.json" --exchange-json "$WORK/run$r-ex.json" \
            --offset-bound-ns "$BOUND_NS" --jitter-json "$JIT" \
            --json-out "$WORK/run$r-report.json" > "$WORK/run$r-report.txt" 2>&1
        rc=$?
        if (( rc == 1 )); then
            say "  run $r: BROKEN"; grep -E "^  !" "$WORK/run$r-report.txt"; exit 1
        fi
        OK=$((OK + 1))
        say "  run $r: ok (report exit $rc)"
    else
        say "  run $r: did not complete"; exit 1
    fi
done

# ---- 8. after: the machine must not have moved under the run -----------------
step "8. post-flight: the machine must be the machine it was"
"$BIN/cpu_jitter" --cpus "$CPU_A,$CPU_B" --seconds "$JITTER_SECONDS" \
    --json "$WORK/jitter-post.json" > "$WORK/jitter-post.txt" 2>&1
if (( $? != 0 )); then say "cpu_jitter (post) failed"; exit 1; fi
"$BIN/tsc_offset" --samples 200000 --cpu-a "$CPU_A" --cpu-b "$CPU_B" \
    --json "$WORK/tsc-offset-post.json" > "$WORK/tsc-offset-post.txt" 2>&1
if (( $? != 0 )); then say "tsc_offset (post) failed"; exit 1; fi
CLOCKSRC_AFTER=""
[[ -r "$CLOCKSRC_PATH" ]] && CLOCKSRC_AFTER="$(cat "$CLOCKSRC_PATH")"
if [[ "$CLOCKSRC_BEFORE" != "$CLOCKSRC_AFTER" ]]; then
    say "REFUSED: clocksource changed under the run: "
    say "         '$CLOCKSRC_BEFORE' -> '$CLOCKSRC_AFTER'"
    exit 1
fi
say "  clocksource unchanged: ${CLOCKSRC_AFTER:-absent}"
POST_JIT=$(python3 - "$WORK/jitter-post.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(sum(c.get("gaps_over", {}).get("100us", 0) or 0 for c in d.get("cpus", [])))
PY
)
say "  post-run gaps over 100us: $POST_JIT"
if [[ "${POST_JIT:-1}" -ne 0 ]]; then
    NOTQ+=("cpu_jitter after the run saw $POST_JIT gaps over 100 us")
fi

# ---- 9. the artifact, always written -----------------------------------------
step "9. artifact"
ART="$OUT/tick-to-trade-$TAG.json"
python3 - "$ART" "$COMMIT" "$DIRTY" "$CPU_A" "$CPU_B" "$BOUND_NS" "$OK" \
        "$CLOCKSRC_BEFORE" "$WORK" "${NOTQ[@]:-}" <<'PY'
import json, os, sys
art, commit, dirty, cpu_a, cpu_b, bound, ok, clocksrc, work = sys.argv[1:10]
notq = [x for x in sys.argv[10:] if x]
runs = []
for name in sorted(os.listdir(work)):
    if name.endswith("-report.json"):
        try:
            runs.append(json.load(open(os.path.join(work, name))))
        except Exception:
            pass
doc = {
    "what": "phase 12.8 tick-to-trade, decomposed",
    "commit": commit,
    "dirty_tree": dirty == "true",
    "cpus": {"exchange": int(cpu_a), "strategy": int(cpu_b)},
    "clocksource": clocksrc or None,
    "tsc_offset_bound_ns": float(bound),
    "completed_runs": int(ok),
    "quotable": len(notq) == 0,
    "not_quotable_because": notq,
    "runs": runs,
    "notes": [
        "The decisive gate is cpu_jitter on the exact pinned pair, not the clock "
        "checks: every clock check this repository owns passes on a WSL2 box "
        "whose scheduler makes a microsecond decomposition meaningless.",
        "Sum(hops) == t_last - t_first is arithmetic, not a check: differences "
        "of stored stamps telescope. Coverage, two-instruments, sign gates and "
        "count identities are what can fail.",
        "Both cross-process hops carry the clock offset with opposite sign, so "
        "their SUM is offset-free and only their SPLIT is exposed. The headline "
        "t1'->t3 is entirely strategy-local and carries none of it.",
    ],
}
json.dump(doc, open(art, "w"), indent=2)
print("  wrote " + art)
PY

if (( ${#NOTQ[@]} )); then
    say ""
    say "=== RAN, NOT QUOTABLE ==="
    for n in "${NOTQ[@]}"; do [[ -n "$n" ]] && say "  - $n"; done
    say ""
    say "  The artifact is written anyway: deleting the evidence is not the same"
    say "  as declining to quote it."
    exit 3
fi
say ""
say "=== QUOTABLE ==="
say "  $OK runs on cpus $CPU_A/$CPU_B, offset bound $BOUND_NS ns, artifacts in $WORK"
exit 0
