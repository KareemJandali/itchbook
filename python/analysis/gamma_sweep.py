#!/usr/bin/env python3
"""gamma_sweep.py — gamma against inventory and against P&L, as a figure.

The plan says the gamma sweep is a FIGURE, not a cherry-picked row, and the two
panels answer different questions with different axes.

  INVENTORY is drawn log-log. It spans decades across the sweep -- that is the
  model working -- and on a linear axis every gamma but the smallest is a flat
  line on the floor.

  P&L is drawn linear WITH ZERO IN RANGE, because P&L is read by its sign. A
  market maker that loses half a cent per share and one that makes half a cent
  are not two points on a scale, and an axis that crops zero makes a small
  positive number look like a large one. svgchart's include_zero exists for
  exactly this.

One line per fill model, because since phase 11.0 every headline number is a
band over four worlds and a single line would be picking one.

Usage:
  gamma_sweep.py validation/as-experiment.json --svg-inventory a.svg --svg-pnl b.svg
                 [--symbol MSFT] [--day 2020-01-30]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from analysis.svgchart import svg_lines, svg_loglog   # noqa: E402

MODELS = ["naive", "optimistic", "mbo", "pessimistic"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("experiment")
    ap.add_argument("--svg-inventory")
    ap.add_argument("--svg-pnl")
    ap.add_argument("--symbol")
    ap.add_argument("--day")
    a = ap.parse_args()

    d = json.load(open(a.experiment))
    symbol = a.symbol or d["symbols"][0]
    day = a.day or (d["evaluation_days"][0] if d["evaluation_days"]
                    else d["calibration_day"])
    gammas = [g for g in d["gammas"] if g > 0]
    if len(gammas) < 2:
        sys.exit(f"need at least two positive gammas to sweep, have {d['gammas']}")

    def series(field):
        out = []
        for m in MODELS:
            vals = []
            for g in gammas:
                hit = [r for r in d["runs"]
                       if r["symbol"] == symbol and r["day"] == day and r["arm"] == "as"
                       and r["model"] == m and abs(r["gamma"] - g) < 1e-12]
                vals.append(hit[0][field] if hit else None)
            if all(v is not None for v in vals):
                out.append((m, vals))
        return out

    note = (f"{symbol} · {day}"
            + ("  (CALIBRATION DAY — not an evaluation result)"
               if day == d["calibration_day"] else "")
            + f" · k = {d['k_measured']:g} · quote size {d['quote_size']}")

    if a.svg_inventory:
        s = series("inv_stdev")
        if not s:
            sys.exit("no inventory data for that symbol-day")
        with open(a.svg_inventory, "w") as f:
            f.write(svg_loglog(gammas, s,
                               "Inventory against risk aversion",
                               note, "γ (per dollar)",
                               "time-weighted inventory σ (shares)"))
        print(f"wrote {a.svg_inventory}")

    if a.svg_pnl:
        s = [(m, [v / 10000.0 for v in vals]) for m, vals in series("equity_per_share_micros")]
        if not s:
            sys.exit("no P&L data for that symbol-day")
        with open(a.svg_pnl, "w") as f:
            # include_zero: P&L is read by its sign, so the axis must show it.
            f.write(svg_lines([f"{g:g}" for g in gammas], s,
                              "P&L per share against risk aversion", note,
                              "γ (per dollar)", "equity per share (cents)",
                              include_zero=True))
        print(f"wrote {a.svg_pnl}")


if __name__ == "__main__":
    main()
