#!/usr/bin/env bash
#
# check-crosses.sh — 9.12's free oracle, over the whole graded basket.
#
#   ./scripts/check-crosses.sh
#
# Reads validation/official-auction-prices.csv, which YOU fill in by reading the
# official opening and closing prices off a published quote history, and runs
# check_cross.py once per symbol-day that has a number to check against.
#
# Why you fill it in and not the tool: the official price is the whole point of
# this check. It is the one number in the project that does not come from our
# own parsing, and a figure supplied by something that has already seen our
# answer is not independent of it. 9.12 says it plainly -- "read those figures
# off the page yourself: supplying them from a chat transcript and then agreeing
# with them is a tautology with extra steps."
#
# Rows with both price columns blank are skipped and reported as skipped, so a
# half-filled file cannot read as a clean pass.
#
# Note the columns say which auctions exist. A symbol that is not NASDAQ-listed
# has no NASDAQ auction at all -- ALLE is in the graded basket and is absent
# from this file for exactly that reason, not by oversight.
set -uo pipefail
cd "$(dirname "$0")/.."

CSV="${1:-validation/official-auction-prices.csv}"
[[ -f "$CSV" ]] || { echo "error: no such file: $CSV" >&2; exit 2; }

ran=0; skipped=0; failed=0
while IFS=, read -r SYM DAY HAS_O HAS_C OPEN CLOSE SOURCE; do
    [[ "$SYM" == "symbol" ]] && continue
    [[ -z "$SYM" ]] && continue
    if [[ -z "$OPEN" && -z "$CLOSE" ]]; then
        echo "  SKIP $SYM $DAY — no official prices filled in"
        skipped=$((skipped + 1))
        continue
    fi
    ARGS=()
    [[ -n "$OPEN"  ]] && ARGS+=(--official-open  "$OPEN")
    [[ -n "$CLOSE" ]] && ARGS+=(--official-close "$CLOSE")
    [[ -n "$SOURCE" ]] && ARGS+=(--source "$SOURCE")
    echo "--- $SYM $DAY"
    python3 python/analysis/check_cross.py "validation/${SYM}_${DAY}.json" "${ARGS[@]}" \
        || failed=$((failed + 1))
    ran=$((ran + 1))
done < <(tr -d '\r' < "$CSV")
# Carriage returns stripped on the way in, not assumed absent. csv.DictWriter
# writes RFC-4180 CRLF, and `read` splits on LF alone, so the CR survived on the
# last field -- which is `source`, which is printed at the end of the verdict
# line. The CR snapped the cursor to column 0 and the trailing full stop
# overwrote the P, so a passing check rendered as ".ASS" on the reader's
# terminal while `od -c` on the same bytes looked perfect. A file edited in
# Excel would have done the same thing, so this strips rather than forbids.

echo
echo "$ran graded, $failed failed, $skipped skipped"
# Skipped rows are not passes. Exit non-zero so a half-filled file cannot be
# mistaken for a finished check.
[[ "$failed" != "0" ]] && exit 1
[[ "$skipped" != "0" ]] && exit 2
exit 0
