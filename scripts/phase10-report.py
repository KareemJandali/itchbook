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
# Mirrors SENDER_BUDGET_NS in bench/rate-sweep.py and kSenderTailBudgetNs in
# tools/mold_replay_udp.cpp.
SENDER_BUDGET_NS = 10000
END = "<!-- generated:end -->"


def n(v, unit=""):
    if v is None:
        return "—"
    return f"{v:,.0f}{unit}"


def clock_row():
    """The cross-core offset, from tsc_offset's own artifact.

    Optional: the sweep can be run without it, and the container this was
    developed in cannot produce a meaningful one. But when it exists it MUST
    come from the file rather than from whoever read the terminal -- the
    wire-to-book sample is a subtraction between two cores' clocks, so the bound
    on their offset is part of the measurement, not a footnote to it.
    """
    art = V / "tsc-offset.json"
    if not art.exists():
        return ["| cross-core clock offset | not measured — "
                "run tools/tsc_offset on the measurement host |"]
    d = json.loads(art.read_text())
    if not d.get("pinned"):
        return ["| cross-core clock offset | **unpinned** — the two stamps did not "
                "come from the two cores being compared |"]
    if d.get("resolvable"):
        return [f"| cross-core clock offset | measured, {d['resolution_ns']:.0f} ns "
                f"resolution ({d['timestamp_source']}) |"]
    return [f"| cross-core clock offset | **bounded under {d['resolution_ns']:.0f} ns**, "
            f"not measured — the estimate is smaller than the method can resolve "
            f"({d['timestamp_source']}) |"]


def build(d):
    feed = d["feed"]
    L = [BEGIN, ""]

    # THE REFUSAL GOES FIRST, IN THE DOCUMENT, not in a terminal nobody keeps.
    #
    # This script used to read sender_qualified_rates only to print it as one
    # row of a table, then emit max-sustainable, knee and cliff underneath as
    # flat statements. A reader arriving at the generated block had no way to
    # tell a measured headline from one taken on a machine whose load generator
    # could not keep a schedule -- and the numbers most likely to be quoted
    # were exactly the ones the sweep had disowned.
    # ALL THREE DISQUALIFYING CONDITIONS, because scripts/pinned-run.sh refuses
    # on all three and used to tell the operator the document had been stamped
    # when only one of them stamped it.
    reasons = []
    if d.get("sender_qualified_rates", 0) == 0:
        reasons.append(
            f"The load generator missed its schedule at every rate: p99.9 lateness "
            f"exceeded {n(SENDER_BUDGET_NS)} ns at all {d['rates_tried']} rungs, "
            "which is the same order as the latency this run exists to measure. "
            "The curve, the knee, the cliff and the max sustainable rate below "
            "therefore cannot be attributed to this pipeline.")
    if not d.get("pinned"):
        reasons.append(
            "The threads were not pinned. Phase 4 measured 19.3% run-to-run "
            "variance on a single-threaded benchmark without pinning, and this "
            "has three threads across two processes.")
    ms = d.get("max_sustainable")
    if ms and ms.get("is_lower_bound"):
        reasons.append(
            "The ladder never found the cliff, so the max sustainable rate below "
            "is a LOWER BOUND — a fact about how high the sweep was told to "
            "count, not about the pipeline.")
    if reasons:
        L += ["> **NOT QUOTABLE.**", ">"]
        for r in reasons:
            L += [f"> - {r}", ">"]
        L += ["> Everything below is printed in full because deleting it would hide "
              "the evidence. None of it may be quoted.", ""]
    elif d["sender_qualified_rates"] < d["rates_tried"]:
        L += [f"> **Partly quotable.** The sender held its schedule at "
              f"{d['sender_qualified_rates']} of {d['rates_tried']} rates; the rows "
              "whose verdict includes *sender late* below measure the generator as "
              "much as the pipeline.", ""]

    L += ["## What was run", "",
          "| | |", "|---|---:|",
          f"| feed | {n(feed['messages'])} messages over "
          f"{feed['session_seconds']:,.1f} s of session |",
          f"| one times real time | {n(feed['real_time_msg_per_s'])} msg/s |",
          f"| ring | {n(d['ring_slots'])} slots |",
          f"| clock | {d['clock']} |"] + clock_row() + [
          f"| threads pinned | {'yes' if d['pinned'] else '**no**'} |",
          f"| runs per rate | {d['repeats']} (best of) |",
          f"| rates on the ladder | {d['rates_tried']} |",
          f"| rates where the sender held its schedule | "
          f"{d['sender_qualified_rates']} of {d['rates_tried']} |",
          ""]

    L += ["## The curve", "",
          "Kernel drops and ring drops are separate columns, never a sum. On "
          "loopback either can overflow first — which one does is a property "
          "of the ring size against the socket buffer, not of loopback — so a "
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
        # ADDITIVE. A row can be both, and the cliff row of the current
        # artifact is: rendering only "**lossy**" hid the sender-late rows the
        # banner above tells the reader to go and find.
        verdict = ", ".join(x for x in ("**lossy**" if r["lossy"] else "",
                                        "sender late" if r["sender_late"] else "")
                            if x) or "clean"
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
    sweep = json.loads(art.read_text())
    block = build(sweep)
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
    ms = sweep.get("max_sustainable")
    if (sweep.get("sender_qualified_rates", 0) == 0 or not sweep.get("pinned")
            or (ms and ms.get("is_lower_bound"))):
        print("  ...and marked it NOT QUOTABLE. The reasons are at the top of the "
              "generated block.", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
