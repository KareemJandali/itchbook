#!/usr/bin/env bash
#
# databento-grade.sh — grade the reconstructed basket against Databento's bars.
#
#   ./scripts/databento-grade.sh              # price it, spend nothing
#   ./scripts/databento-grade.sh --spend      # actually query and grade
#
# 9.12's third item: ">= 5 symbols exact vs Databento ... repeated on a second
# day", spanning the liquidity spectrum. The basket below is that spectrum, and
# every symbol in it is already reconstructed and committed under validation/,
# with the C++ book and the Python oracle agreeing on each.
#
# COSTS MONEY. That is why the default does nothing but ask what it would cost:
# --cost-only calls metadata.get_cost and bills nothing, and the totals print
# before anything is spent. Nothing here runs without --spend.
#
# Requires DATABENTO_API_KEY in the environment. This script never reads a key
# from an argument, so it cannot end up in a shell history file.
set -uo pipefail
cd "$(dirname "$0")/.."

if [[ -z "${DATABENTO_API_KEY:-}" ]]; then
    echo "error: DATABENTO_API_KEY is not set." >&2
    echo "       export it in your shell, then re-run. It is never passed as an" >&2
    echo "       argument here, so it stays out of your shell history." >&2
    exit 2
fi

SPEND=0
[[ "${1:-}" == "--spend" ]] && SPEND=1

# symbol:date -- the spectrum 9.12 asks for, on two days. MKD had not listed in
# August and ELTK is August's halted name, so the halted slot differs by day.
BASKET=(
    "MSFT:2019-12-30"  "QQQ:2019-12-30"  "ALLE:2019-12-30"  "AQB:2019-12-30"  "MKD:2019-12-30"
    "MSFT:2019-08-30"  "QQQ:2019-08-30"  "ALLE:2019-08-30"  "AQB:2019-08-30"  "ELTK:2019-08-30"
)

echo "=== what this would bill ==="
for entry in "${BASKET[@]}"; do
    SYM="${entry%%:*}"; DAY="${entry##*:}"
    printf '  %-6s %s  ' "$SYM" "$DAY"
    python3 python/analysis/validate.py "validation/${SYM}_${DAY}.json" \
        --symbol "$SYM" --date "$DAY" --cost-only 2>&1 | tail -1
done

if [[ "$SPEND" != "1" ]]; then
    echo
    echo "Nothing was spent. Re-run with --spend to query and grade."
    exit 0
fi

echo
echo "=== grading ==="
fail=0
for entry in "${BASKET[@]}"; do
    SYM="${entry%%:*}"; DAY="${entry##*:}"
    echo "--- $SYM $DAY"
    ORACLE="validation/databento-${SYM}-${DAY}.json"
    # Pay once. If the response is already committed, replay it for nothing --
    # the verdict is the same and the artifact is what the claim rests on.
    if [[ -f "$ORACLE" ]]; then
        python3 python/analysis/validate.py "validation/${SYM}_${DAY}.json" \
            --symbol "$SYM" --date "$DAY" --oracle-json "$ORACLE" || fail=$((fail + 1))
    else
        python3 python/analysis/validate.py "validation/${SYM}_${DAY}.json" \
            --symbol "$SYM" --date "$DAY" --save-oracle "$ORACLE" || fail=$((fail + 1))
    fi
done

echo
if [[ "$fail" != "0" ]]; then
    echo "$fail of ${#BASKET[@]} symbol-days did not match."
    exit 1
fi
echo "all ${#BASKET[@]} symbol-days exact against Databento"
