#!/usr/bin/env python3
"""The global invariants: the census and the book must agree in aggregate.

    python3 scripts/full-day-check.py \
        --census validation/census-2019-12-30.json \
        --run    validation/all-symbols-2019-12-30.json \
        --per-symbol validation/all-symbols-2019-12-30.csv

The oracle differential proves a symbol right, one symbol at a time, and cannot
be run over 268 million messages. These checks are the other half: they cannot
tell you a symbol is right, and they catch the class of bug sampling misses --
correct per symbol, wrong in aggregate. A book that dropped one message type, or
double-counted a locate, or lost volume between symbols, passes every per-symbol
differential and fails here.

Every number compared comes from a committed artifact. Nothing is typed in.
Exit code 1 on any disagreement, so this is a gate rather than a report.
"""
import argparse
import csv
import json
import sys
from pathlib import Path

# 'H', 'S' and 'R' carry state but mutate nothing, so dispatch's apply()
# returns false for them and the book does not count them as applied. Any
# accounting of read-minus-applied has to know that or it is off by their sum.
MODELLED_BUT_NOT_MUTATING = ("H", "S", "R")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--census", required=True)
    ap.add_argument("--run", required=True)
    ap.add_argument("--per-symbol")
    args = ap.parse_args()

    census = json.loads(Path(args.census).read_text())
    run = json.loads(Path(args.run).read_text())
    live = census["live_orders"]
    types = census.get("types")

    checks = []
    skipped = []

    def check(name, got, want, note=""):
        checks.append((name, got, want, got == want, note))

    def skip(name, why):
        # A check that did not run must not look like a check that passed.
        # Two of these went quiet on the first real run -- the census predated
        # the type histogram -- and the only thing that said so was the count.
        skipped.append((name, why))

    check("messages read", run["messages_read"], census["messages"],
          "the book and the census must have seen the same file")

    if not types:
        skip("messages skipped", "census has no type histogram; re-run itch_census")
        skip("books built", "census has no type histogram; re-run itch_census")
    if types:
        modelled = {"S", "R", "A", "F", "E", "C", "X", "D", "U", "P", "Q", "H"}
        unmodelled = sum(n for t, n in types.items() if t not in modelled)
        inert = sum(types.get(t, 0) for t in MODELLED_BUT_NOT_MUTATING)
        check("messages skipped", run["messages_read"] - run["messages_applied"],
              unmodelled + inert,
              f"{unmodelled:,} unmodelled + {inert:,} modelled-but-inert (H/S/R)")
        check("books built", run["books"], types.get("R", 0),
              "one book per stock directory entry, no more and no fewer")

    # Every order that ever rested was added or replaced into place. The census
    # counts that from the wire with no book at all; the book counts it while
    # building one. Two routes to the same number.
    check("orders added", run["adds"], live["adds"] + live["replaces"],
          "adds + replaces, counted independently by a structure with no levels")
    check("resting at the close", run["resting_orders"], live["final"],
          "the day ends with an empty book, or it does not")

    check("unknown references", run["unknown_refs"], 0,
          "every reference in the feed named an order the book was holding")
    check("locate mismatches", run["locate_mismatch"], 0,
          "no reference resolved to another symbol's order")
    check("undirectoried messages", run["undirectoried_messages"], 0,
          "every message routed to a book had a directory entry")

    if not args.per_symbol:
        for name in ("per-symbol rows", "summed volume", "summed adds", "summed off-band"):
            skip(name, "no --per-symbol file given")
    if args.per_symbol:
        rows = list(csv.DictReader(Path(args.per_symbol).open()))
        check("per-symbol rows", len(rows), run["books"])
        check("summed volume", sum(int(r["volume"]) for r in rows), run["executed_volume"],
              "the per-symbol file and the run summary describe one day")
        check("summed adds", sum(int(r["adds"]) for r in rows), run["adds"])
        check("summed off-band", sum(int(r["off_band_adds"]) for r in rows),
              run["off_band_adds"])

    width = max(len(c[0]) for c in checks)
    failed = 0
    for name, got, want, ok, note in checks:
        mark = "ok " if ok else "FAIL"
        print(f"  {mark} {name:<{width}} {got:>15,}" + (f" != {want:,}" if not ok else ""))
        if note and not ok:
            print(f"       {note}")
        failed += 0 if ok else 1

    for name, why in skipped:
        print(f"  SKIP {name:<{width}} {why}")

    if failed:
        print(f"\n{failed} global invariant(s) failed")
        return 1
    print(f"\nOK: {len(checks)} global invariants hold across "
          f"{run['messages_read']:,} messages and {run['books']:,} symbols")
    if skipped:
        # Exit 2, not 0. Ten checks passing out of twelve is not the same claim
        # as twelve checks passing, and the only thing that said so on the first
        # real run was a count nobody was reading.
        print(f"    {len(skipped)} did NOT run -- see SKIP above. They are not passes.")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
