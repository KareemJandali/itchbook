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


def sensitivity(vals):
    """How much of the zero-latency number survives the slowest point.

    Reported as a fraction rather than a difference so it can be compared
    across strategies whose absolute P&L differs by orders of magnitude. A
    negative ratio means the sign flipped, which is worth saying out loud
    rather than burying in a percentage.
    """
    first, last = vals[0], vals[-1]
    if first == 0:
        return None
    return last / first


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

    out.append("surviving fraction of the zero-latency number "
               "(1.0 = flat, 0.0 = the edge was latency)")
    for name, m in by.items():
        r_pnl = sensitivity(m["per_share"])
        r_sh = sensitivity([float(v) for v in m["shares"]])
        pnl_s = "n/a" if r_pnl is None else f"{r_pnl:+.2f}"
        sh_s = "n/a" if r_sh is None else f"{r_sh:.2f}"
        out.append(f"  {name:<13} c/share {pnl_s:>7}   shares {sh_s:>6}")
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
