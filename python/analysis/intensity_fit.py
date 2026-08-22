#!/usr/bin/env python3
"""intensity_fit.py — draw the fill-intensity curve and the place it fails.

Two series per lane: the OBSERVED ln lambda per depth bucket, and the FITTED
exponential. The gap between them is the residual, and drawing them together
rather than plotting residuals alone is deliberate -- a residual plot shows that
something is wrong, while this shows what the model believed and what actually
happened, which is the difference between a diagnostic and an argument.

The touch bucket is where they part company. Avellaneda-Stoikov assumes fill
intensity depends only on depth; at depth zero it depends mostly on QUEUE
POSITION, which the model has no way to express. That misfit is a known
limitation of A-S and this is the figure that shows it rather than asserting it.

Reads the JSON that tools/calibrate_intensity writes.

Usage:
  intensity_fit.py validation/intensity.json --svg docs/figures/intensity.svg
                   [--lane mbo]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from analysis.svgchart import svg_lines   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("calibration")
    ap.add_argument("--svg", required=True)
    ap.add_argument("--lane", default="mbo",
                    help="which fill model's curve to draw (default mbo, the "
                         "queue-position-resolved one)")
    a = ap.parse_args()

    d = json.load(open(a.calibration))
    lanes = d["lanes"]
    if a.lane not in lanes:
        sys.exit(f"no lane '{a.lane}' in {a.calibration}; have {sorted(lanes)}")
    lane = lanes[a.lane]
    if not lane.get("fit_ok"):
        sys.exit(f"lane '{a.lane}' has no usable fit "
                 f"({lane['buckets_fitted']} buckets fitted, "
                 f"{lane['buckets_no_fills']} with exposure but no fills)")

    res = lane["residuals"]
    if len(res) < 2:
        sys.exit(f"only {len(res)} fitted points; nothing to draw")

    xs = [str(r["ticks"]) for r in res]
    series = [("observed", [r["observed"] for r in res]),
              ("fitted A·e^(−kδ)", [r["fitted"] for r in res])]

    worst = min(res, key=lambda r: r["residual"])
    touch = next((r for r in res if r["ticks"] == 0), None)

    # The caveats travel with the chart, because a figure gets pasted into a
    # slide without the paragraph that qualified it.
    sub = (f"{a.lane} lane · A = {lane['A']:.3g} fills/order-second · "
           f"k = {lane['k']:.0f} per dollar · weighted R² = {lane['r_squared']:.3f} · "
           f"{lane['maker_fills']:,} maker fills over "
           f"{lane['exposure_order_seconds']:,.0f} order-seconds")
    if lane["buckets_no_fills"]:
        sub += f" · {lane['buckets_no_fills']} deeper buckets had exposure but no fills"

    with open(a.svg, "w") as f:
        f.write(svg_lines(xs, series,
                          "Fill intensity against depth, measured not assumed",
                          sub, "depth from mid (ticks)", "ln λ (fills per order-second)"))
    print(f"wrote {a.svg}")

    if touch is not None and touch["residual"] < -0.3:
        print(f"  touch bucket sits {-touch['residual']:.2f} log-units below the fit — "
              "queue position, which A-S cannot express")
    print(f"  worst residual {worst['residual']:+.2f} at {worst['ticks']} ticks")


if __name__ == "__main__":
    main()
