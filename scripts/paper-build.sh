#!/usr/bin/env bash
#
# paper-build.sh — the paper, in the one order that is correct.
#
#   scripts/paper-build.sh            # build it
#   scripts/paper-build.sh --check    # fail if anything is stale
#
# THE ORDER IS LOAD-BEARING AND IT IS NOT OBVIOUS, which is why this file
# exists instead of three commands in a README.
#
#   1. paper-figures.sh   draws the figures from the committed JSON
#   2. paper-report.py    writes the tables AND links whichever figures exist
#   3. paper-html.py      renders the Markdown, inlining those figures
#
# Step 2 reads what step 1 produced. Run them the other way round and the report
# silently omits a figure that is about to appear -- the document is not wrong in
# any way you can see, it is merely different from what the same inputs produce
# next time. That is exactly what standing rule 7's --check is for, and exactly
# how it caught this: the committed paper had no intensity figure, regeneration
# from the same artifacts had one, and verify-local called it stale.
#
# A three-step pipeline whose steps must run in an order nobody wrote down is a
# pipeline that will be run in the wrong order. Now there is one entry point.
set -uo pipefail
cd "$(dirname "$0")/.."

check=""
[[ "${1:-}" == "--check" ]] && check="--check"

fail=0
run() {
    if ! "$@"; then
        echo "  FAILED: $*"
        fail=1
    fi
}

if [[ -n $check ]]; then
    run ./scripts/paper-figures.sh --check
    run python3 scripts/paper-report.py --check
    run python3 scripts/paper-html.py --check
else
    run ./scripts/paper-figures.sh
    run python3 scripts/paper-report.py
    run python3 scripts/paper-html.py
fi

echo
if [[ $fail -ne 0 ]]; then
    echo "paper build FAILED"
    exit 1
fi
[[ -n $check ]] && echo "paper is current" || echo "paper built"
