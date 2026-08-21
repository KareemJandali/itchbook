#!/usr/bin/env python3
"""Generate the results tables in docs/phase10-results.md from the sweep's artifacts.

    python3 scripts/phase10-report.py            # write it
    python3 scripts/phase10-report.py --check    # fail if it is stale

Standing rule 7. Every figure between the generated markers is read from a file
a tool wrote; none is typed. The prose around them is written by hand, because a
conclusion is not a number and should not pretend to be generated.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
V = ROOT / "validation"
OUT = ROOT / "docs" / "phase10-results.md"
BEGIN = "<!-- generated:begin -->"
END = "<!-- generated:end -->"


def n(v, unit=""):
    if v is None:
        return "—"
    return f"{v:,.0f}{unit}"


def build(d):
    feed = d["feed"]
    L = [BEGIN, ""]

    L += ["## What was run", "",
          "| | |", "|---|---:|",
          f"| feed | {n(feed['messages'])} messages over "
          f"{feed['session_seconds']:,.1f} s of session |",
          f"| one times real time | {n(feed['real_time_msg_per_s'])} msg/s |",
          f"| ring | {n(d['ring_slots'])} slots |",
          f"| clock | {d['clock']} |",
          f"| threads pinned | {'yes' if d['pinned'] else '**no**'} |",
          f"| runs per rate | {d['repeats']} (best of) |",
          f"| rates on the ladder | {d['rates_tried']} |",
          f"| rates where the sender held its schedule | "
          f"{d['sender_qualified_rates']} of {d['rates_tried']} |",
          ""]

    L += ["## The curve", "",
          "Kernel drops and ring drops are separate columns, never a sum. On "
          "loopback the socket buffer overflows before the ring does, so a "
          "table that added them could not tell a pipeline that is drowning "
          "from one whose water came in upstream.", "",
          "Offered is what the sender was told to send; achieved is what it "
          "managed. They are the same number only while it keeps its schedule, "
          "and the pipeline can only have absorbed the second one.", "",
          "| offered | achieved | × real time | p50 ns | p99 ns | p99.9 ns | ring-full | "
          "kernel | mid-block | peak occupancy | sender p99.9 late | verdict |",
          "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|"]
    for r in d["rows"]:
        w = r["wire_to_book_ns"]
        k = r["kernel_drops"]
        verdict = ("**lossy**" if r["lossy"]
                   else ("sender late" if r["sender_late"] else "clean"))
        L.append(
            f"| {n(r['offered_rate'])} | {n(r.get('achieved_rate'))} | "
            f"{r['multiplier']:g}× | {n(w['p50'])} | "
            f"{n(w['p99'])} | {n(w['p999'])} | {n(r['ring_full_drops'])} | "
            f"{'UNKNOWN' if k is None else n(k)} | {n(r['staging_overflow'])} | "
            f"{n(r['peak_ring_occupancy'])} | "
            f"{n(r['sender']['lateness_ns']['p999'])} | {verdict} |")
    L.append("")

    L += ["## The two annotations", "", "| | |", "|---|---|"]
    s = d.get("max_sustainable")
    if s is None:
        L.append("| max sustainable rate | **none** — the lowest rate on the ladder "
                 "already dropped |")
    else:
        bound = "≥ " if s["is_lower_bound"] else ""
        L.append(f"| max sustainable rate | {bound}{n(s.get('achieved_rate'))} msg/s "
                 f"achieved ({n(s['offered_rate'])} offered, "
                 f"{s['multiplier']:g}× real time) |")
        L.append(f"| ...at which | p50 {n(s['p50_ns'])} ns, p99 {n(s['p99_ns'])} ns, "
                 f"p99.9 {n(s['p999_ns'])} ns |")
        if s["is_lower_bound"]:
            L.append("| why a bound | the top of the ladder was still clean, so this "
                     "is where counting stopped, not where the pipeline stopped |")
    k = d.get("knee")
    if k is None:
        L.append("| knee | not reached — p99 never doubled over the flat region |")
    else:
        L.append(f"| knee | {n(k['offered_rate'])} msg/s ({k['multiplier']:g}× real "
                 f"time), p99 {n(k['p99_ns'])} ns against a "
                 f"{n(d['baseline_p99_ns'])} ns baseline |")
    c = d.get("cliff")
    if c is None:
        L.append("| cliff | **not reached** |")
    else:
        L.append(f"| cliff | {n(c['offered_rate'])} msg/s ({c['multiplier']:g}×) — "
                 f"{n(c['ring_full_drops'])} ring-full, "
                 f"{'UNKNOWN' if c['kernel_drops'] is None else n(c['kernel_drops'])} "
                 f"kernel, {n(c['staging_overflow'])} mid-block |")
    L += ["", END]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    art = V / "rate-sweep.json"
    if not art.exists():
        sys.exit(f"error: missing artifact {art}")
    block = build(json.loads(art.read_text()))
    text = OUT.read_text()
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    updated = head + block + tail
    if args.check:
        if updated != text:
            print("docs/phase10-results.md is stale. "
                  "Run: python3 scripts/phase10-report.py")
            return 1
        print("phase 10 results match their artifacts")
        return 0
    OUT.write_text(updated)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
