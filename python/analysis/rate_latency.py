#!/usr/bin/env python3
"""rate_latency.py — draw the rate-latency curve and the distribution beside it.

Two figures, because they answer different questions and neither substitutes
for the other:

  * The CURVE says where the pipeline stops keeping up. Three percentiles
    against offered rate, log-log, with the knee and the max sustainable rate
    drawn as vertical rules rather than left to a caption.
  * The HISTOGRAM says what the latency at the max sustainable rate actually
    LOOKS like. Two pipelines can share a p50 and a p99 and have completely
    different shapes -- one tight cluster with stragglers, or two modes because
    a branch is taken half the time. The second is a mechanism you can go and
    find; the percentile table hides it. That is the same argument
    histogram.hpp makes for emitting buckets at all, and phase 10.7 asks for
    the distribution alongside the curve for exactly this reason.

Reads the JSON bench/rate-sweep.py writes. Nothing here recomputes a number:
the knee and the sustainable rate are decided by the sweep, from the runs, and
this only draws them.

Usage:
  rate_latency.py out/rate-sweep.json --svg out/rate-latency.svg
                  [--hist-svg out/wire-to-book-hist.svg]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from analysis.svgchart import svg_loglog, svg_histogram   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sweep")
    ap.add_argument("--svg", required=True)
    ap.add_argument("--hist-svg")
    a = ap.parse_args()

    d = json.load(open(a.sweep))
    rows = d["rows"]
    if not rows:
        sys.exit("the sweep recorded no rows")

    # Plotted against ACHIEVED rate, not offered. Above the knee the sender
    # starts missing its schedule, and a curve drawn against what it was asked
    # for stretches the x axis by exactly the amount the generator fell short --
    # which moves the knee to the right and flatters the pipeline.
    xs = [r.get("achieved_rate") or r["offered_rate"] for r in rows]
    series = [("p50", [r["wire_to_book_ns"]["p50"] for r in rows]),
              ("p99", [r["wire_to_book_ns"]["p99"] for r in rows]),
              ("p99.9", [r["wire_to_book_ns"]["p999"] for r in rows])]

    rules = []
    if d.get("max_sustainable"):
        s = d["max_sustainable"]
        rules.append((f"max sustainable {s.get('achieved_rate') or s['offered_rate']:,.0f}/s "
                      f"({s['multiplier']:g}x)",
                      s.get("achieved_rate") or s["offered_rate"]))
    if d.get("knee"):
        k = d["knee"]
        rules.append((f"knee {k.get('achieved_rate') or k['offered_rate']:,.0f}/s "
                      f"({k['multiplier']:g}x)",
                      k.get("achieved_rate") or k["offered_rate"]))

    feed = d["feed"]
    # The caveats travel WITH the chart. A figure that has to be read next to a
    # paragraph to be understood correctly gets separated from that paragraph
    # the first time someone pastes it into a slide.
    caveat = "pinned" if d.get("pinned") else "UNPINNED — describes a scheduler as much as a pipeline"
    sub = (f"{feed['messages']:,} messages, {feed['real_time_msg_per_s']:,.0f} msg/s at 1x "
           f"real time; ring {d['ring_slots']:,} slots; best of {d['repeats']}; {caveat}")
    with open(a.svg, "w") as f:
        f.write(svg_loglog(xs, series, "Wire-to-book latency against offered rate", sub,
                           "offered rate (messages/second)", "latency (nanoseconds)",
                           rules=rules))
    print(f"wrote {a.svg}")

    if a.hist_svg:
        s = d.get("max_sustainable")
        if not s or not s.get("buckets"):
            print("no bucket data at the max sustainable rate; skipping the histogram")
            return
        markers = [("p50", s["p50_ns"]), ("p99", s["p99_ns"]), ("p99.9", s["p999_ns"])]
        # The buckets are in CYCLES as the harness recorded them; the markers
        # are in nanoseconds. Convert the markers rather than the buckets, so
        # the bars stay the counts the tool actually emitted.
        cyc_per_ns = d["rows"][0].get("cycles_per_ns") or 1.0
        markers = [(n, v * cyc_per_ns) for n, v in markers]
        with open(a.hist_svg, "w") as f:
            f.write(svg_histogram(
                [tuple(b) for b in s["buckets"]],
                "Wire-to-book distribution at the max sustainable rate",
                f"{s['offered_rate']:,.0f} msg/s ({s['multiplier']:g}x real time); "
                f"{caveat}",
                markers=markers, x_label="cycles, wire to book", y_label="messages"))
        print(f"wrote {a.hist_svg}")


if __name__ == "__main__":
    main()
