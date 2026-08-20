#!/usr/bin/env python3
"""Adverse selection: what the market did right after it filled you.

    ./build/queue_backtest data/sliced/MSFT.gz --json results.json
    python3 python/analysis/markout.py results.json --svg docs/markout.svg

A markout is the mid at `t + h` minus the mid at your fill, signed so that
POSITIVE is in your favour on whichever side you traded. It answers the one
question a P&L number cannot: were you filled because you offered a fair price,
or because someone knew the price was about to move?

  * **Drift near zero at every horizon.** Your counterparties were, on average,
    uninformed. A maker can live here.
  * **Drift negative and getting worse with the horizon.** You are being picked
    off, and the effect compounds as the information arrives. This is the
    normal case for a naive passive strategy and it is what eats the rebate.
  * **Drift positive.** Suspect it before believing it. On a passive strategy
    it usually means the fill model is filling you at prices the queue would
    never have reached — check `through` and `lock` fills first.

Two things this chart deliberately does NOT do:

  * It does not drop unresolved fills. A fill near the end of the session has
    no 10-second future in the data, and quietly excluding those biases the
    long horizon towards whatever the middle of the day looked like. The count
    is printed instead, and a horizon where it is large should be distrusted.
  * It does not connect the three horizons with a smooth curve implying
    measurements in between. They are three separate measurements; the line is
    there to show the trend across them and the markers are the data.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from svgchart import svg_lines   # noqa: E402

MODEL_ORDER = ["naive", "optimistic", "mbo", "pessimistic"]
HORIZONS = [("100ms", "drift_100ms"), ("1s", "drift_1s"), ("10s", "drift_10s")]


def load(path):
    d = json.loads(Path(path).read_text())
    models = d["models"]
    rows = []
    for name in MODEL_ORDER:
        if name not in models:
            continue
        m = models[name]
        rows.append({
            "model": name,
            "fills": m["fills"],
            "shares": m["shares"],
            "drift": [m[key] / 10000.0 for _, key in HORIZONS],
            "edge": (m["edge_micros"] / m["shares"] / 10000.0) if m["shares"] else 0.0,
            "through": m.get("through_fills", 0),
            "unresolved": m.get("unresolved_10s", 0),
            "lock": m.get("lock_fills", 0),
        })
    return d, rows


def ascii_table(d, rows):
    out = [f"markouts — {d.get('strategy', '?')}  ({d.get('events', 0):,} events)", ""]
    out.append(f"{'model':<14}{'edge c/sh':>11}" +
               "".join(f"{h:>11}" for h, _ in HORIZONS) +
               f"{'unresolved':>12}{'through':>10}{'lock':>7}")
    out.append("-" * (25 + 11 * len(HORIZONS) + 29))
    for r in rows:
        cells = "".join(f"{v:>11.4f}" for v in r["drift"])
        out.append(f"{r['model']:<14}{r['edge']:>11.4f}{cells}"
                   f"{r['unresolved']:>12}{r['through']:>10}{r['lock']:>7}")
    out.append("")
    out.append("drift is cents/share, signed so positive is in your favour.")
    out.append("negative and worsening with the horizon = being picked off.")
    out.append("`unresolved` is fills with no 10s future left in the data. They are "
               "counted,")
    out.append("not dropped, so a large count means the 10s column is thin rather "
               "than wrong.")

    # The comparison that matters: does the spread you captured survive the
    # adverse selection that came with it?
    out.append("")
    out.append("edge + drift at the longest horizon (what the fill was actually worth)")
    for r in rows:
        net = r["edge"] + r["drift"][-1]
        verdict = "survives" if net > 0 else "eaten"
        out.append(f"  {r['model']:<13} {net:+9.4f} c/share   {verdict}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", help="queue_backtest --json output")
    ap.add_argument("--svg", help="write the markout chart here")
    ap.add_argument("--no-ascii", action="store_true")
    a = ap.parse_args()

    d, rows = load(a.results)
    if not rows:
        raise SystemExit("error: no model results in that file")
    if not a.no_ascii:
        print(ascii_table(d, rows))

    if a.svg:
        series = [(r["model"], r["drift"]) for r in rows]
        Path(a.svg).parent.mkdir(parents=True, exist_ok=True)
        Path(a.svg).write_text(svg_lines(
            [h for h, _ in HORIZONS], series,
            f"Post-fill drift — {d.get('strategy', '?')}",
            "Positive is in your favour. Negative and worsening with the horizon "
            "means you are being picked off.",
            "horizon after the fill (three separate measurements)",
            "cents/share"))
        print(f"\nwrote {a.svg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
