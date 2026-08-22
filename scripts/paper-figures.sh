#!/usr/bin/env bash
#
# paper-figures.sh — every figure in docs/paper/as-on-itch.md, from committed JSON.
#
#   scripts/paper-figures.sh            # regenerate them
#   scripts/paper-figures.sh --check    # fail if any is stale, write nothing
#
# Standing rule 7 says no NUMBER reaches a document by being retyped. A figure is
# a document full of numbers that happens to be drawn, and it decays the same
# way: someone regenerates the JSON, forgets the chart, and the paper carries a
# picture of last month's data with this month's caption. So the same rule
# applies, and CI runs --check.
#
# There is a second rule here that the number-generators do not need:
#
#   A FIGURE WHOSE ARTIFACT IS MISSING IS AN ORPHAN, AND AN ORPHAN IS A FAILURE.
#
# Not a skip. If docs/figures/paper/gamma-pnl.svg is committed and
# validation/as-experiment.json is not, then there is a chart in the paper that
# nothing in the repository can reproduce or refute -- which is exactly the
# artifact-backed-by-nothing failure that took CI red four times in phase 11.
# When the artifact is absent the figure must be absent too, and the paper's
# generated blocks then say "not measured" rather than linking a picture.
#
# Every figure records its provenance in docs/figures/paper/manifest.json: the
# artifact it came from, that artifact's SHA-256, and the exact command. The
# manifest is checked like the figures.
set -uo pipefail
cd "$(dirname "$0")/.."

check=0
[[ "${1:-}" == "--check" ]] && check=1

FIG=docs/figures/paper
# One calibration artifact per symbol now, so the figure is drawn from the
# first one and the manifest records WHICH -- an intensity curve is per symbol
# and a caption that does not say which symbol is a caption that is wrong.
CALIB=$(ls validation/intensity*.json 2>/dev/null | head -1)
CALIB=${CALIB:-validation/intensity.json}
EXPT=validation/as-experiment.json
LANE=mbo

fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/out"

entries=()   # figure<TAB>artifact<TAB>command<TAB>subject, for the manifest

note()  { printf '  %s\n' "$1"; }
bad()   { printf '  FAILED: %s\n' "$1"; fail=1; }

# An artifact is missing: its figures must be missing too.
orphan_guard() {
    local artifact=$1; shift
    local f
    for f in "$@"; do
        if [[ -e "$FIG/$f" ]]; then
            bad "$FIG/$f exists but $artifact does not — a figure nothing can reproduce"
        fi
    done
    note "skipped (no $artifact): ${*}"
}

# Generated into $tmp/out; installed or diffed depending on mode.
settle() {
    local f
    for f in "$@"; do
        if [[ ! -s "$tmp/out/$f" ]]; then
            bad "generator produced no $f"
            continue
        fi
        if [[ $check -eq 1 ]]; then
            if [[ ! -e "$FIG/$f" ]]; then
                bad "$FIG/$f is missing; run scripts/paper-figures.sh"
            elif ! cmp -s "$tmp/out/$f" "$FIG/$f"; then
                bad "$FIG/$f is stale; run scripts/paper-figures.sh"
            else
                note "$f matches its artifact"
            fi
        else
            mkdir -p "$FIG"
            cp "$tmp/out/$f" "$FIG/$f"
            note "wrote $FIG/$f"
        fi
    done
}

printf '=== fill intensity (§6.2)\n'
if [[ -f "$CALIB" ]]; then
    cmd="python3 python/analysis/intensity_fit.py $CALIB --svg $FIG/intensity-$LANE.svg --lane $LANE"
    if python3 python/analysis/intensity_fit.py "$CALIB" \
            --svg "$tmp/out/intensity-$LANE.svg" --lane "$LANE" >/dev/null; then
        settle "intensity-$LANE.svg"
        csym=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('symbol','?'))" "$CALIB")
        cday=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('day','?'))" "$CALIB")
        entries+=("intensity-$LANE.svg	$CALIB	$cmd	$csym · $cday · $LANE lane")
    else
        bad "intensity_fit.py failed on $CALIB"
    fi
else
    orphan_guard "$CALIB" "intensity-$LANE.svg"
fi

printf '=== the gamma sweep (§7.4)\n'
if [[ -f "$EXPT" ]]; then
    # The symbol-day is chosen HERE and recorded, rather than left to the
    # plotter's default, so the caption in the paper and the data in the chart
    # cannot come from two different rules.
    read -r sym day < <(python3 - "$EXPT" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
day = d["evaluation_days"][0] if d["evaluation_days"] else d["calibration_day"]
print(d["symbols"][0], day)
PY
)
    cmd="python3 python/analysis/gamma_sweep.py $EXPT --symbol $sym --day $day --svg-inventory $FIG/gamma-inventory.svg --svg-pnl $FIG/gamma-pnl.svg"
    if python3 python/analysis/gamma_sweep.py "$EXPT" --symbol "$sym" --day "$day" \
            --svg-inventory "$tmp/out/gamma-inventory.svg" \
            --svg-pnl "$tmp/out/gamma-pnl.svg" >/dev/null; then
        settle gamma-inventory.svg gamma-pnl.svg
        entries+=("gamma-inventory.svg	$EXPT	$cmd	$sym · $day" "gamma-pnl.svg	$EXPT	$cmd	$sym · $day")
    else
        bad "gamma_sweep.py failed on $EXPT"
    fi
else
    orphan_guard "$EXPT" gamma-inventory.svg gamma-pnl.svg
fi

printf '=== provenance\n'
# The entries go through a FILE, not a pipe. `python3 - <<PY` takes its script
# from stdin, so a pipe into it is silently discarded -- which is how the first
# version of this wrote an empty manifest next to three freshly drawn figures
# and then reported "no figures, no manifest -- consistent".
printf '%s\n' "${entries[@]+"${entries[@]}"}" > "$tmp/entries.tsv"
python3 - "$tmp/manifest.json" "$tmp/entries.tsv" <<'PY'
import hashlib, json, os, sys
figs = []
for line in open(sys.argv[2]).read().splitlines():
    if not line.strip():
        continue
    fig, artifact, cmd, subject = line.split("\t")
    h = hashlib.sha256(open(artifact, "rb").read()).hexdigest()
    figs.append({"figure": fig, "artifact": artifact, "artifact_sha256": h,
                 "subject": subject, "command": cmd})
json.dump({"figures": sorted(figs, key=lambda f: f["figure"])},
          open(sys.argv[1], "w"), indent=1)
open(sys.argv[1], "a").write("\n")
PY
if [[ $check -eq 1 ]]; then
    if [[ ! -e "$FIG/manifest.json" ]]; then
        # No figures and no manifest is the honest empty state.
        if [[ -s "$tmp/manifest.json" ]] && \
           ! python3 -c "import json,sys;sys.exit(0 if not json.load(open(sys.argv[1]))['figures'] else 1)" "$tmp/manifest.json"; then
            bad "$FIG/manifest.json is missing; run scripts/paper-figures.sh"
        else
            note "no figures, no manifest — consistent"
        fi
    elif ! cmp -s "$tmp/manifest.json" "$FIG/manifest.json"; then
        bad "$FIG/manifest.json is stale; run scripts/paper-figures.sh"
    else
        note "manifest matches"
    fi
else
    if python3 -c "import json,sys;sys.exit(0 if json.load(open(sys.argv[1]))['figures'] else 1)" "$tmp/manifest.json"; then
        mkdir -p "$FIG"
        cp "$tmp/manifest.json" "$FIG/manifest.json"
        note "wrote $FIG/manifest.json"
    else
        rm -f "$FIG/manifest.json"
        note "no figures to record"
    fi
fi

echo
if [[ $fail -ne 0 ]]; then
    echo "paper figures FAILED"
    exit 1
fi
if [[ $check -eq 1 ]]; then
    echo "paper figures match their artifacts"
else
    echo "paper figures regenerated"
fi
