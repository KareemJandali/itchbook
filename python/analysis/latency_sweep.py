#!/usr/bin/env python3
"""P&L against flight time — the shape matters more than any single point.

    ./build/latency_sweep data/sliced/MSFT.gz --strategy touch-maker \
        --csv bench/latency.csv
    python3 python/analysis/latency_sweep.py bench/latency.csv \
        --svg docs/latency.svg

A backtest quoted at one latency is a point estimate wearing a result's
clothes. The curve is the object worth looking at, and it says one of two
things:

  * **Flat.** The strategy is not racing anyone. Whatever it earns survives a
    slower machine, and the number is a claim about the strategy rather than
    about the co-location contract behind it.
  * **Collapsing.** The P&L lived in the first few hundred microseconds. That
    is a latency edge, and claiming it means owning the infrastructure to hold
    it. Most retail-adjacent "alpha" that is gone by 1ms was never alpha.

Read the two panels together. A curve that is flat in cents per share while the
share count falls is not a strategy that survived latency — it is a strategy
that traded less and kept its average, which is a different and much smaller
claim. Fill volume is on the second panel for exactly that reason.

Here colour IS identity: one line per fill model, four genuine series, each
also labelled at its end so the chart survives being printed in grey.
"""
import argparse
import csv
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from svgchart import svg_lines   # noqa: E402

MODEL_ORDER = ["naive", "optimistic", "mbo", "pessimistic"]


def load(path):
    """CSV -> (strategy, [latency_ns...], {model: {metric: [value per latency]}}).

    The tool writes micro-dollars because that is the unit the ledger counts in;
    converting once here and never again keeps every number downstream in cents
    and keeps the rounding in one place.
    """
    rows = list(csv.DictReader(open(path, newline="")))
    if not rows:
        raise SystemExit(f"error: {path} has no rows")
    strategy = rows[0]["strategy"]
    lats = sorted({int(r["latency_ns"]) for r in rows})
    by = OrderedDict()
    for name in MODEL_ORDER:
        pts = {int(r["latency_ns"]): r for r in rows if r["model"] == name}
        if not pts:
            continue
        by[name] = {
            "per_share": [int(pts[l]["per_share_micros"]) / 10000.0 for l in lats],
            "edge_per_share": [int(pts[l]["edge_per_share_micros"]) / 10000.0 for l in lats],
            "shares": [int(pts[l]["shares"]) for l in lats],
            "fills": [int(pts[l]["fills"]) for l in lats],
            "total": [int(pts[l]["equity_micros"]) / 1000000.0 for l in lats],
        }
    return strategy, lats, by


def us_labels(lats):
    return [("0" if l == 0 else f"{l / 1000:g}") for l in lats]


def change(vals):
    """(first, last, delta) from the fastest point to the slowest.

    A DIFFERENCE, not a ratio. The ratio was here first and it was wrong. On a
    strategy that loses money the P&L is negative at both ends, so a loss that
    grew by 77% came out as "1.77" under a heading that said 1.0 meant flat —
    the worst possible reading of the worst possible outcome. A signed delta in
    the metric's own units says the same thing in both regimes and needs no
    heading to disambiguate it.
    """
    return vals[0], vals[-1], vals[-1] - vals[0]


def ratio(vals):
    """Last over first, for quantities that cannot be negative.

    Share counts only. Never P&L.
    """
    return None if vals[0] == 0 else vals[-1] / vals[0]


def ascii_table(strategy, lats, by):
    out = [f"latency sweep — {strategy}", ""]
    heads = "".join(f"{u + 'us':>12}" for u in us_labels(lats))
    for metric, label, fmt in (("shares", "shares filled", "{:>12,}"),
                               ("per_share", "P&L (cents/share)", "{:>12.4f}"),
                               ("total", "P&L ($)", "{:>12.2f}")):
        out.append(label)
        out.append(f"{'model':<13}{heads}")
        out.append("-" * (13 + 12 * len(lats)))
        for name, m in by.items():
            cells = "".join(fmt.format(v) for v in m[metric])
            out.append(f"{name:<13}{cells}")
        out.append("")

    lo_us, hi_us = us_labels(lats)[0], us_labels(lats)[-1]
    out.append(f"change from {lo_us}us to {hi_us}us")
    out.append(f"  {'model':<13}{'c/share':>10}{'delta':>10}{'shares':>10}")
    for name, m in by.items():
        first, last, delta = change(m["per_share"])
        sh = ratio([float(v) for v in m["shares"]])
        sh_s = "n/a" if sh is None else f"x{sh:.2f}"
        out.append(f"  {name:<13}{first:>10.4f}{delta:>+10.4f}{sh_s:>10}")
    out.append("")
    out.append("A flat c/share delta means the strategy is not racing anyone. "
               "Read it WITH the")
    out.append("share column: for a strategy that loses money, filling more is "
               "worse, so a")
    out.append("rising share count and a worsening delta are the same fact "
               "twice.")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="latency_sweep --csv output")
    ap.add_argument("--svg", help="write the P&L-vs-latency chart here")
    ap.add_argument("--shares-svg", help="write the fill-volume panel here")
    ap.add_argument("--metric", default="per_share",
                    choices=["per_share", "edge_per_share", "total"],
                    help="which P&L view the line chart draws (default per_share; "
                         "totals reward whichever model fills most, which is the "
                         "difference under study)")
    ap.add_argument("--no-ascii", action="store_true")
    a = ap.parse_args()

    strategy, lats, by = load(a.csv)
    if not a.no_ascii:
        print(ascii_table(strategy, lats, by))

    xs = us_labels(lats)
    unit = {"per_share": "cents/share", "edge_per_share": "edge, cents/share",
            "total": "dollars"}[a.metric]
    if a.svg:
        series = [(n, m[a.metric]) for n, m in by.items()]
        Path(a.svg).parent.mkdir(parents=True, exist_ok=True)
        Path(a.svg).write_text(svg_lines(
            xs, series, f"P&L vs one-way latency — {strategy}",
            "Flat means the strategy is not racing anyone. Collapsing means the "
            "edge was the wire.",
            "one-way latency, microseconds (sweep points, evenly spaced — not to scale)",
            unit))
        print(f"\nwrote {a.svg}")
    if a.shares_svg:
        series = [(n, [float(v) for v in m["shares"]]) for n, m in by.items()]
        Path(a.shares_svg).parent.mkdir(parents=True, exist_ok=True)
        Path(a.shares_svg).write_text(svg_lines(
            xs, series, f"Fill volume vs one-way latency — {strategy}",
            "Read with the P&L panel: a flat average on falling volume is a "
            "smaller claim than it looks.",
            "one-way latency, microseconds (sweep points, evenly spaced — not to scale)",
            "shares filled"))
        print(f"wrote {a.shares_svg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
