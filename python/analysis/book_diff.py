#!/usr/bin/env python3
"""Diff two book-snapshot CSVs — the phase 3 differential test.

The C++ `book_replay` and the Python reference must produce byte-identical
snapshot files for the same input. "Close" is not a passing grade: a book that
is one share off on one level at one instant is a book that is wrong, and the
whole point of keeping a slow oracle around is to catch exactly that.

    python3 python/analysis/book_diff.py reference.csv candidate.csv

Exits 0 when the files agree, 1 when they do not, and reports the first
divergence with enough context to start debugging: which row, which column,
which timestamp, and both values.
"""
import argparse
import sys


def load(path):
    with open(path) as f:
        rows = [line.rstrip("\n") for line in f]
    if not rows:
        raise SystemExit(f"error: {path} is empty")
    return rows[0].split(","), [r.split(",") for r in rows[1:]]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", help="the oracle's CSV")
    ap.add_argument("candidate", help="the CSV under test")
    ap.add_argument("--max-report", type=int, default=5,
                    help="how many divergences to print before stopping (default 5)")
    a = ap.parse_args(argv)

    ref_hdr, ref_rows = load(a.reference)
    cand_hdr, cand_rows = load(a.candidate)

    if ref_hdr != cand_hdr:
        print("HEADER MISMATCH")
        print(f"  reference: {','.join(ref_hdr)}")
        print(f"  candidate: {','.join(cand_hdr)}")
        return 1

    problems = 0
    for i, (r, c) in enumerate(zip(ref_rows, cand_rows), start=1):
        if r == c:
            continue
        ts = r[0] if r else "?"
        if len(r) != len(c):
            print(f"row {i} (ts={ts}): field count {len(r)} != {len(c)}")
            problems += 1
        else:
            for col, (rv, cv) in enumerate(zip(r, c)):
                if rv != cv:
                    name = ref_hdr[col] if col < len(ref_hdr) else f"col{col}"
                    print(f"row {i} (ts={ts}) {name}: reference={rv} candidate={cv}")
                    problems += 1
                    if problems >= a.max_report:
                        break
        if problems >= a.max_report:
            print(f"... stopping after {problems} divergences")
            break

    if len(ref_rows) != len(cand_rows):
        print(f"row count: reference={len(ref_rows)} candidate={len(cand_rows)}")
        problems += 1

    if problems:
        print(f"FAIL: {a.reference} and {a.candidate} disagree")
        return 1

    print(f"OK: {len(ref_rows)} snapshot rows identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
