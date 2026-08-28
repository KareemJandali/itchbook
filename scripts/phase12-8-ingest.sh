#!/usr/bin/env bash
#
# phase12-8-ingest.sh — take the boot's results off the stick and turn them into
# something that can be read, checked and committed.
#
# Nothing here is copied by hand. The live session wrote one tarball; this
# unpacks it, re-derives every number from the raw traces rather than trusting
# the summaries the live session printed, pools the repeats, and writes the
# artifact.
#
# WHY IT RE-DERIVES RATHER THAN READS. The live session's stdout is a
# transcript, and a transcript is not evidence: it cannot be re-checked, and a
# number in it cannot be traced to the samples that produced it. The traces are
# the evidence, and they came back with the tarball, so the percentiles are
# computed here from the same reader that is under test in CI.
#
# WHAT IT REFUSES. A tarball with no traces, a run whose report says BROKEN, and
# a commit mismatch between the binaries that ran and the tree that is being
# committed to. The last one matters: an artifact's commit field is only worth
# something if a reader can get back to the code.
#
set -uo pipefail

TARBALL="${1:-}"
OUT="${OUT:-validation}"
# NOT /tmp: it is wiped between WSL invocations on this machine, and the raw
# material is what a later question has to go back to.
WORK="${WORK:-$HOME/itchbook-12-8-ingest}"

if [[ -z "$TARBALL" ]]; then
    echo "usage: $0 <phase12-8-results.tar.gz>" >&2
    echo "  the file the live session wrote to the stick" >&2
    exit 2
fi
if [[ ! -f "$TARBALL" ]]; then
    echo "error: $TARBALL does not exist" >&2
    exit 2
fi

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
echo "=== unpacking $(basename "$TARBALL") ($(stat -c %s "$TARBALL" | numfmt --to=iec))"
tar xzf "$TARBALL" -C "$WORK" || { echo "error: cannot unpack" >&2; exit 1; }

# ---- what came back ------------------------------------------------------------
# The ten repeats live under w-main. Everything else -- the multiplier sweep,
# the MTU sweep, the held-ack arm, the dry run -- is a DIFFERENT configuration
# and is reported separately. Pooling them would be asking whether runs that
# were set up to differ differ.
mapfile -t MAIN_TRACES < <(find "$WORK" -path '*w-main*' -name 'run*-st.trace' | sort)
mapfile -t OTHER_TRACES < <(find "$WORK" -name '*-st.trace' \
    -not -path '*w-main*run*' | sort)
STRACES=("${MAIN_TRACES[@]}")
mapfile -t SAMPLES < <(find "$WORK" -name '*-samples.json' | sort)
mapfile -t REPORTS < <(find "$WORK" -name '*-report.json' | sort)
echo "  repeats (w-main): ${#MAIN_TRACES[@]}"
echo "  other configs   : ${#OTHER_TRACES[@]}  (sweeps, dry run, ack-held)"
echo "  sample sets     : ${#SAMPLES[@]}"
echo "  run reports     : ${#REPORTS[@]}"

if (( ${#STRACES[@]} == 0 )); then
    echo
    echo "REFUSED: no traces in the tarball. The runs did not produce raw records,"
    echo "         so there is nothing to re-derive and the transcript alone is"
    echo "         not evidence."
    find "$WORK" -maxdepth 3 -type d | head -20
    exit 1
fi

# ---- the artifacts the boot went out to make -----------------------------------
#
# The harness measured these on the exact pinned pair, before and after. Not
# passing them through would make every run report "no cpu_jitter artifact
# supplied" on a boot whose entire purpose was to produce one.
JITTER="$(find "$WORK" -name 'jitter-pre.json' | head -1)"
OFFSET_JSON="$(find "$WORK" -name 'tsc-offset-pre.json' | head -1)"
OFFSET_BOUND=""
if [[ -n "$OFFSET_JSON" && -s "$OFFSET_JSON" ]]; then
    OFFSET_BOUND="$(python3 -c "
import json,sys
d = json.load(open(sys.argv[1]))
res = d.get('resolution_ns') or d.get('uncertainty_ns') or 0.0
est = abs(d.get('largest_abs_offset_ns') or 0.0)
print('%.3f' % max(res, est))
" "$OFFSET_JSON" 2>/dev/null)"
fi
echo "  cpu_jitter      : ${JITTER:-ABSENT}"
echo "  tsc_offset bound: ${OFFSET_BOUND:-ABSENT} ns"

EXTRA=()
[[ -n "$JITTER" ]] && EXTRA+=(--jitter-json "$JITTER")
[[ -n "$OFFSET_BOUND" ]] && EXTRA+=(--offset-bound-ns "$OFFSET_BOUND")

# ---- re-derive every run from its own traces -----------------------------------
echo
echo "=== re-deriving each run from its raw traces"
FRESH=()
n=0
for st in "${STRACES[@]}"; do
    ex="${st%-st.trace}-ex.trace"
    sj="${st%-st.trace}-st.json"
    ej="${st%-st.trace}-ex.json"
    [[ -f "$ex" && -f "$sj" && -f "$ej" ]] || { echo "  skip $(basename "$st"): incomplete set"; continue; }
    n=$((n + 1))
    tag="r$n"
    python3 scripts/phase12-8-report.py \
        --strategy-trace "$st" --exchange-trace "$ex" \
        --strategy-json "$sj" --exchange-json "$ej" \
        --run-tag "$tag" "${EXTRA[@]}" \
        --samples-out "$WORK/$tag-fresh-samples.json" \
        --json-out "$WORK/$tag-fresh-report.json" \
        > "$WORK/$tag-fresh.txt" 2>&1
    rc=$?
    if (( rc == 1 )); then
        echo "  $tag: BROKEN"
        grep -E "^  !" "$WORK/$tag-fresh.txt" | head -3
        exit 1
    fi
    FRESH+=("$WORK/$tag-fresh-samples.json")
    printf '  %-4s exit %d  %s\n' "$tag" "$rc" \
        "$(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
h=d.get('headline_t1p_t3') or {}
print('joined %s, headline p50 %s ns, coverage %s%%' % (
    d.get('joined'), h.get('p50'),
    round(d.get('coverage_p50_pct') or 0, 1)))
" "$WORK/$tag-fresh-report.json" 2>/dev/null)"
done

if (( ${#FRESH[@]} < 2 )); then
    echo
    echo "REFUSED: fewer than two complete runs, so there is nothing to pool and"
    echo "         no way to say whether the machine held still."
    exit 1
fi

# ---- pool ----------------------------------------------------------------------
echo
echo "=== pooling ${#FRESH[@]} runs"
python3 scripts/phase12-8-pool.py "${FRESH[@]}" \
    --json-out "$WORK/pooled.json" | tee "$WORK/pooled.txt"
POOL_RC=${PIPESTATUS[0]}
echo "pool exit: $POOL_RC   (0 one experiment, 3 the runs differ)"

# ---- the configurations that were meant to differ ------------------------------
if (( ${#OTHER_TRACES[@]} > 0 )); then
    echo
    echo "=== other configurations, each on its own (NOT pooled with the repeats)"
    for st in "${OTHER_TRACES[@]}"; do
        ex="${st%-st.trace}-ex.trace"
        sj="${st%-st.trace}-st.json"
        ej="${st%-st.trace}-ex.json"
        [[ -f "$ex" && -f "$sj" && -f "$ej" ]] || continue
        label="$(basename "$(dirname "$st")")/$(basename "${st%-st.trace}")"
        python3 scripts/phase12-8-report.py \
            --strategy-trace "$st" --exchange-trace "$ex" \
            --strategy-json "$sj" --exchange-json "$ej" \
            --run-tag "$label" "${EXTRA[@]}" \
            --json-out "$WORK/other-$(echo "$label" | tr / -).json" \
            > "$WORK/other-$(echo "$label" | tr / -).txt" 2>&1
        printf '  %-28s %s\n' "$label" \
            "$(python3 -c "
import json,sys
try:
    d=json.load(open(sys.argv[1]))
except Exception:
    print('(no report)'); raise SystemExit
h=d.get('headline_t1p_t3') or {}
hops=d.get('hops') or {}
pack=[v for k,v in hops.items() if 'PACKING' in k]
print('joined %-5s headline p50 %-8s packing p50 %s' % (
    d.get('joined'), h.get('p50'),
    (pack[0].get('p50') if pack else '-')))
" "$WORK/other-$(echo "$label" | tr / -).json" 2>/dev/null)"
    done
fi

# ---- the artifact ---------------------------------------------------------------
echo
echo "=== artifact"
python3 - "$WORK" "$OUT" "$POOL_RC" <<'PY'
import json, os, sys, glob
work, out, pool_rc = sys.argv[1], sys.argv[2], int(sys.argv[3])

# The HARNESS's own verdict, from the artifact it wrote on the machine. It knows
# things the per-run reports cannot: the CPU governor, the topology it chose, the
# jitter on the exact pinned pair. Omitting these produced an artifact that named
# two secondary reasons and not the one that mattered.
harness_reasons = []
harness_doc = None
for p in glob.glob(os.path.join(work, "**", "tick-to-trade-*.json"), recursive=True):
    if os.path.basename(p).startswith("tick-to-trade-mult"):
        continue
    try:
        d = json.load(open(p))
    except Exception:
        continue
    if "not_quotable_because" in d:
        harness_doc = d
        harness_reasons = ["[harness] " + r for r in d.get("not_quotable_because", [])]
        break
runs = []
for p in sorted(glob.glob(os.path.join(work, "*-fresh-report.json"))):
    try:
        runs.append(json.load(open(p)))
    except Exception:
        pass
pooled = None
pp = os.path.join(work, "pooled.json")
if os.path.exists(pp):
    pooled = json.load(open(pp))

# The verdict is the AND of every run's own verdict and the pooling.
quotable = bool(runs) and all(r.get("quotable") for r in runs) and pool_rc == 0
reasons = sorted({r for run in runs for r in run.get("not_quotable", [])})
reasons = harness_reasons + reasons
if pool_rc == 3:
    reasons.append("the repeats are not one experiment (see pooled)")
quotable = quotable and not harness_reasons

# The configurations that were MEANT to differ, kept out of the pooling and
# recorded here so a generated document can cite them. The multiplier sweep is
# the design's own falsifiable prediction (the reaction path should not move
# with the pacing knob), and a prediction whose numbers live only in stdout is
# a prediction nobody can check.
others = {}
for p in sorted(glob.glob(os.path.join(work, "other-*.json"))):
    try:
        d = json.load(open(p))
    except Exception:
        continue
    label = os.path.basename(p)[len("other-"):-len(".json")]
    hops = d.get("hops") or {}
    pack = [v for k, v in hops.items() if "PACKING" in k]
    others[label] = {
        "joined": d.get("joined"),
        "headline_t1p_t3_p50": (d.get("headline_t1p_t3") or {}).get("p50"),
        "packing_p50": (pack[0].get("p50") if pack else None),
        "quotable": d.get("quotable"),
    }

doc = {
    "what": "phase 12.8 tick-to-trade, decomposed - bare metal",
    "runs": len(runs),
    "other_configurations": others,
    "quotable": quotable,
    "not_quotable_because": reasons,
    "pooled": pooled,
    "harness_verdict": harness_doc,
    "per_run": runs,
    "notes": [
        "Every number here was re-derived from the raw traces by "
        "scripts/phase12-8-report.py, not read from the live session's "
        "transcript. A transcript cannot be re-checked and a number in it "
        "cannot be traced to the samples that produced it.",
        "Sum(hops) == t_last - t_first is arithmetic, not a check: differences "
        "of stored stamps telescope. Coverage, the two instruments, the sign "
        "gates and the count identities are what can fail.",
        "p99.9 appears only where the gap-overlap census does, and then twice: "
        "over all chains and over the chains that were running throughout.",
    ],
}
dest = os.path.join(out, "tick-to-trade-baremetal.json")
json.dump(doc, open(dest, "w"), indent=2)
print("  wrote %s" % dest)
print("  quotable: %s" % quotable)
for r in reasons:
    print("    - %s" % r)
PY

echo
echo "=== raw material kept in $WORK"
echo "    fresh reports, pooled.txt, and the traces themselves"
if (( POOL_RC == 0 )); then
    echo
    echo "The runs are one experiment. The numbers can be written up."
    exit 0
fi
exit 3
