#!/usr/bin/env python3
"""Render the per-message latency distribution book_bench measured.

    ./build/book_bench data/raw/bench.gz --histogram out/latency.csv
    python3 python/analysis/latency_histogram.py out/latency.csv \
        --svg docs/figures/latency-histogram.svg

The build plan asks for a latency histogram, and the reason it asks rather than
settling for the percentile table is that a table of five numbers cannot show a
shape. p50 and p99 are identical between a distribution with one tight mode and
a few stragglers and one with two separate modes fifty cycles apart — and the
second is a mechanism you can go and find, usually a branch taken half the time
or a level that allocates on some messages and not others.

The markers put the percentiles back on top of the shape they came from, so the
table and the picture are visibly the same measurement rather than two claims
the reader has to reconcile.

A text table is printed alongside the SVG, unconditionally: an image is not
readable by everyone or by anything that greps, and the numbers behind a chart
should never live only inside it.
"""
import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from svgchart import svg_histogram   # noqa: E402


def read_buckets(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            lo, hi, count = int(row["lo"]), int(row["hi"]), int(row["count"])
            if count > 0:
                rows.append((lo, hi, count))
    if not rows:
        raise SystemExit(f"error: {path} has no non-empty buckets")
    return rows


def percentile(buckets, pct):
    """Bucket-resolution percentile: the lower bound of the bucket the sample
    falls in. Deliberately not interpolated — pretending to a precision the
    buckets do not have would make these disagree with book_bench's exact
    percentiles for a reason nobody could see."""
    total = sum(c for _, _, c in buckets)
    want = pct / 100.0 * total
    seen = 0
    for lo, _, count in buckets:
        seen += count
        if seen >= want:
            return lo
    return buckets[-1][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="bucket CSV from book_bench --histogram")
    ap.add_argument("--svg", help="write the chart here")
    ap.add_argument("--title", default="Per-message book latency")
    ap.add_argument("--subtitle", default="")
    a = ap.parse_args()

    buckets = read_buckets(a.csv)
    total = sum(c for _, _, c in buckets)
    p50, p99, p999 = (percentile(buckets, p) for p in (50, 99, 99.9))
    lo = buckets[0][0]
    hi = max(b[1] for b in buckets)

    subtitle = a.subtitle or (
        f"{total:,} messages, {lo:,}-{hi:,} cycles, {len(buckets)} occupied buckets")

    print(f"{'bucket (cycles)':>22}  {'count':>12}  share")
    print("-" * 52)
    for blo, bhi, count in buckets:
        share = 100.0 * count / total
        bar = "#" * max(1, round(share / 2)) if share >= 0.5 else ""
        print(f"{blo:>9,} - {bhi:>8,}  {count:>12,}  {share:5.2f}% {bar}")
    print("-" * 52)
    print(f"{'total':>22}  {total:>12,}")
    print(f"\np50 {p50:,}   p99 {p99:,}   p99.9 {p999:,}   (bucket resolution)")

    if a.svg:
        markers = [("p50", p50), ("p99", p99), ("p99.9", p999)]
        Path(a.svg).parent.mkdir(parents=True, exist_ok=True)
        Path(a.svg).write_text(
            svg_histogram(buckets, a.title, subtitle, markers=markers))
        print(f"wrote {a.svg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
