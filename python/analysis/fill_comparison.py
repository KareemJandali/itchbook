#!/usr/bin/env python3
"""The Phase 6 headline: same strategy, same data, four fill models.

The gap between `naive` and `pessimistic` is the point of the whole project.
`mbo` sitting inside that band is what says the band is honest.

Stdlib only — the repo has no plotting dependency and is not acquiring one for a
bar chart. Output is an SVG (for the README) and an ASCII table (for a terminal),
because both audiences matter and neither should need the other's tooling.

    ./build/queue_backtest data/sliced/MSFT.gz --json results.json
    python3 python/analysis/fill_comparison.py results.json --svg docs/fills.svg

Chart decisions worth stating, because each is a rule rather than a taste:

  * Whatever the metric, it is ONE measure across four categories, so every bar
    is ONE colour. Colouring each bar differently would imply the models are
    four series, and reads as decoration.
  * The default panel is TOTAL P&L, because on this data that is where the
    difference lives. The four models agree to within about a tenth of a cent
    per share and disagree by 60% on how many shares were filled at all, so a
    per-share panel on its own shows four identical bars and hides the entire
    result. `--metric per_share` and `--metric shares` are the two halves of
    that total and both are worth drawing; the ASCII table always prints all
    three, so no single chart choice can bury a column.
  * Every bar is directly labelled. Two of the palette's hues sit under 3:1
    against the light surface, and the rule for that is visible labels or a
    table view; this ships both.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from svgchart import fmt_for, svg_bars   # noqa: E402

MODEL_ORDER = ["naive", "optimistic", "mbo", "pessimistic"]

def ascii_table(rows, title):
    out = [title, ""]
    out.append(f"{'model':<14}{'fills':>8}{'shares':>10}{'P&L ($)':>12}"
               f"{'c/share':>12}{'edge c/sh':>12}{'fees c/sh':>12}")
    out.append("-" * 80)
    for r in rows:
        out.append(f"{r['model']:<14}{r['fills']:>8}{r['shares']:>10,.0f}"
                   f"{r['total']:>12.2f}{r['per_share']:>12.4f}"
                   f"{r['edge']:>12.4f}{r['fees']:>12.4f}")

    # Both halves, because either alone can be read as the whole answer.
    for metric, label in (("total", "total P&L"), ("shares", "shares filled")):
        out.append("")
        out.append(label)
        scale = max((abs(r[metric]) for r in rows), default=1.0) or 1.0
        for r in rows:
            n = int(round(abs(r[metric]) / scale * 34))
            bar = ("#" * n) if r[metric] >= 0 else ("-" * n)
            out.append(f"  {r['model']:<13}|{bar}")

    # The headline number: what the flattering assumption is worth.
    #
    # A DIFFERENCE, not a ratio. The ratio was here first and it was wrong: on a
    # strategy that loses money both totals are negative, so naive/pessimistic
    # came out at 0.87 and read as "naive is more conservative" when naive was
    # in fact reporting a loss $401 smaller. A ratio of two negative numbers
    # says nothing about which is flattering; a signed difference says it in
    # both regimes.
    by = {r["model"]: r for r in rows}
    if "naive" in by and "pessimistic" in by:
        gap = by["naive"]["total"] - by["pessimistic"]["total"]
        out.append("")
        if gap > 0:
            out.append(f"naive reports ${gap:,.2f} MORE than pessimistic — that is "
                       f"what assuming you are")
            out.append(f"at the front of every queue is worth, on this data, to "
                       f"this strategy.")
        elif gap < 0:
            out.append(f"naive reports ${-gap:,.2f} LESS than pessimistic. That is "
                       f"backwards for a model that")
            out.append(f"fills strictly more, and it means the extra fills are "
                       f"losing more than they make —")
            out.append(f"worth understanding before quoting either number.")
        else:
            out.append("naive and pessimistic agree exactly, which on real data "
                       "would be surprising.")
        if by["pessimistic"]["total"] > 0 and by["naive"]["total"] > 0:
            # Only meaningful when both are profits; otherwise it inverts.
            ratio = by["naive"]["total"] / by["pessimistic"]["total"]
            out.append(f"({ratio:.2f}x)")
    return "\n".join(out)


def load(path):
    d = json.loads(Path(path).read_text())
    models = d["models"]
    rows = []
    for name in MODEL_ORDER:
        if name not in models:
            continue
        m = models[name]
        shares = m["shares"] or 1
        rows.append({
            "model": name,
            "fills": m["fills"],
            "shares": float(m["shares"]),
            "per_share": m["per_share_micros"] / 10000.0,
            "total": m["equity_micros"] / 1000000.0,
            "edge": m["edge_micros"] / shares / 10000.0,
            "fees": m["fees_micros"] / shares / 10000.0,
            "drift_1s": m["drift_1s"] / 10000.0,
        })
    return d, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", help="queue_backtest --json output")
    ap.add_argument("--svg", help="write the headline bar chart here")
    ap.add_argument("--metric", default="total",
                    choices=["total", "per_share", "shares"],
                    help="what the bars encode (default total; per_share alone "
                         "can show four identical bars while the models "
                         "disagree by 60%% on fill volume)")
    ap.add_argument("--no-ascii", action="store_true")
    a = ap.parse_args()

    d, rows = load(a.results)
    if not rows:
        raise SystemExit("error: no model results in that file")

    heading = {"total": "Total P&L by fill model",
               "per_share": "P&L per share by fill model",
               "shares": "Shares filled by fill model"}[a.metric]
    unit = {"total": "dollars", "per_share": "cents/share",
            "shares": "shares"}[a.metric]
    title = f"{heading} — {d.get('strategy', '?')}"
    sub = (f"{d.get('events', 0):,} events. The gap between naive and pessimistic "
           f"is the queue assumption; mbo should sit inside it.")

    if not a.no_ascii:
        print(ascii_table(rows, title))

    if a.svg:
        Path(a.svg).parent.mkdir(parents=True, exist_ok=True)
        values = [r[a.metric] for r in rows]
        Path(a.svg).write_text(
            svg_bars([r["model"] for r in rows], values, title, sub,
                     unit=unit, value_fmt=fmt_for(values)))
        print(f"\nwrote {a.svg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
