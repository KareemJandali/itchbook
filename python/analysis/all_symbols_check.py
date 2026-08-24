#!/usr/bin/env python3
"""Every symbol at once must equal every symbol one at a time.

    python3 python/analysis/all_symbols_check.py data/raw/fuzz.gz --binary build/book_replay

Phase 9 routes a whole feed into 8,700 books sharing one reference map and one
pool. Nothing about that is supposed to change what any individual book does —
the mutations are the same code, reached through one extra switch — so the way
to find out whether routing broke a symbol is to reconstruct that symbol both
ways and diff the results.

This is not the oracle differential and does not replace it. The oracle answers
"is the book right", which still runs per symbol on a slice. This answers a
narrower question the oracle cannot see: given that the single-symbol path is
graded correct, did putting it behind a BookSet move anything. A shared pool
handing out the wrong node, a locate routing a message to its neighbour, a
counter that became the market's instead of the symbol's — all of those are
invisible to a run that only ever builds one book, and all of them show up here
as a field that stopped matching.

Exit code 1 on any disagreement, so it can be a CI step rather than a report.
"""
import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

# The fields both paths report. Everything here is cumulative over the session
# except the two touch prices, which are the state at the end of it — between
# them they cover "what happened" and "where it finished".
FIELDS = [
    "resting_orders", "resting_shares", "volume", "notional", "trades",
    "hidden_volume", "cross_volume", "open", "high", "low", "close",
    "best_bid", "best_ask", "unknown_refs",
]


def as_opt_int(text):
    """The CSV writes an absent price as an empty cell; the JSON writes null."""
    return None if text == "" else int(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed")
    ap.add_argument("--binary", default="build/book_replay")
    ap.add_argument("--tick", type=int, default=100)
    ap.add_argument("--max-symbols", type=int, default=0,
                    help="check only the first N symbols; 0 checks every one")
    ap.add_argument("--band-levels", type=int, default=512,
                    help="dense band slots per side for the all-symbols run")
    ap.add_argument("--band-sweep", type=int, nargs="*", default=None,
                    help="also assert the reconstruction is identical at each of "
                         "these band widths. The band is a locality knob: where a "
                         "level is stored must not change any number reported.")
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    feed = Path(args.feed).resolve()
    if not binary.exists():
        sys.exit(f"error: no such binary: {binary}")

    with tempfile.TemporaryDirectory() as tmp:
        per_symbol = Path(tmp) / "per_symbol.csv"
        def all_symbols(out, levels):
            subprocess.run([str(binary), str(feed), "--all-symbols", "--per-symbol",
                            str(out), "--tick", str(args.tick),
                            "--band-levels", str(levels), "--quiet"], check=True)

        all_symbols(per_symbol, args.band_levels)

        # The band decides whether a level lives in a dense slot or in the cold
        # overflow map, and nothing else. Every column except the ones that
        # describe the band itself must survive changing it -- which is what
        # makes the width a budget decision rather than a correctness one.
        #
        # peak_overflow_levels belongs here for the same reason overflow_levels
        # does, and more obviously: it is a high-water mark of the overflow map,
        # so narrowing the band necessarily raises it. Adding the column without
        # adding it here turned a correct reconstruction into a failed sweep at
        # width 8 -- the check was right and the list was stale.
        band_fields = {"overflow_levels", "peak_overflow_levels",
                       "off_band_adds", "recentres"}
        if args.band_sweep:
            base = [{k: v for k, v in r.items() if k not in band_fields}
                    for r in csv.DictReader(per_symbol.open())]
            for levels in args.band_sweep:
                other = Path(tmp) / f"band_{levels}.csv"
                all_symbols(other, levels)
                rows = [{k: v for k, v in r.items() if k not in band_fields}
                        for r in csv.DictReader(other.open())]
                if rows != base:
                    differing = next((i for i, (a, b) in enumerate(zip(rows, base)) if a != b), None)
                    sys.exit(f"band width {levels} changed the reconstruction "
                             f"(first differing row {differing})")
            print(f"band sweep: identical at widths "
                  f"{args.band_levels} and {', '.join(str(x) for x in args.band_sweep)}")

        rows = list(csv.DictReader(per_symbol.open()))
        if not rows:
            sys.exit("error: --all-symbols produced no rows; nothing was checked")

        checked = 0
        skipped = []
        failures = []
        for row in rows:
            symbol = row["symbol"]
            # A locate with no 'R' cannot be asked for by name, so the
            # single-symbol path has no way to reconstruct it. Reported rather
            # than dropped: "checked 8,000 of 8,700" is a different claim from
            # "checked them all".
            if row["directoried"] != "yes" or not symbol:
                skipped.append(row["locate"])
                continue
            if args.max_symbols and checked >= args.max_symbols:
                break

            out = Path(tmp) / f"{symbol}.json"
            subprocess.run([str(binary), str(feed), "--symbol", symbol, "--json", str(out),
                            "--tick", str(args.tick), "--quiet"], check=True)
            one = json.loads(out.read_text())

            for field in FIELDS:
                both = one.get(field)
                many = as_opt_int(row[field])
                if both != many:
                    failures.append(f"{symbol} (locate {row['locate']}): {field} "
                                    f"all-symbols={many} single={both}")
            checked += 1

        # Zero on any feed that is what it claims to be. Checked here rather
        # than only in the summary, because a per-symbol row is where you would
        # find out WHICH symbol.
        for row in rows:
            if int(row["locate_mismatch"]) != 0:
                failures.append(f"{row['symbol']}: locate_mismatch="
                                f"{row['locate_mismatch']}, must be 0")

    if skipped:
        print(f"skipped {len(skipped)} locate(s) with no directory entry: "
              f"{', '.join(skipped[:10])}{' ...' if len(skipped) > 10 else ''}")
    if failures:
        print(f"\nFAIL: {len(failures)} disagreement(s) over {checked} symbol(s)")
        for f in failures[:40]:
            print(f"  {f}")
        return 1
    print(f"OK: {checked} symbol(s), {len(FIELDS)} fields each — "
          f"all-symbols and single-symbol agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
