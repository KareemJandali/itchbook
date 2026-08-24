#!/usr/bin/env python3
"""Diff two daily-summary JSONs — the other half of the differential test.

    python3 python/analysis/summary_diff.py reference.json candidate.json

`book_diff.py` compares the snapshot CSVs, which prove the two books agree at
the instants they sample. This compares what is cumulative BETWEEN those
instants: volume, OHLC, VWAP, resting orders and shares at the close. A book can
produce identical snapshots at every sampled instant and still have miscounted
in the gaps, and only this catches that.

This lived as a heredoc inside `full-day-differential.sh` until a second caller
needed it. There is one implementation of the comparison for the same reason
`report.hpp` has one writer: two copies of a comparison fail on a formatting
difference, or -- much worse -- agree because both made the same mistake.

Exits 0 when the summaries agree, 1 when they do not.
"""
import argparse
import json
import sys

# The Python driver's own bookkeeping. These say nothing about whether the two
# BOOKS agree, so they are named rather than counted.
DRIVER_ONLY = ("symbol", "session_end_ns")


def compare(a, b):
    """Return (shared_keys, differing_keys, driver_only_keys)."""
    keys = sorted(set(a) & set(b))
    return keys, [k for k in keys if a[k] != b[k]], sorted(set(a) ^ set(b))


def report(a, b, quiet=False):
    keys, bad, only = compare(a, b)
    if not quiet:
        width = max(len(k) for k in keys) if keys else 0
        for k in keys:
            flag = "" if a[k] == b[k] else "<-- DIFFER"
            print(f"  {k:<{width}}  {str(a[k]):>18}  {str(b[k]):>18}  {flag}")
        if only:
            print(f"\n  (driver-only fields, not compared: {', '.join(only)})")
        if bad:
            print("\nFAIL: the summaries disagree on " + ", ".join(bad))
        else:
            print(f"\n{len(keys)} summary fields identical")
    return keys, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    a = json.loads(open(args.reference).read())
    b = json.loads(open(args.candidate).read())
    _, bad = report(a, b, args.quiet)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
