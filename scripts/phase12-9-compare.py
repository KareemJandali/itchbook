#!/usr/bin/env python3
"""phase12-9-compare.py — the replay-vs-live A/B, with every disagreement categorised.

WHY THIS IS NOT A PER-FILL DIFF, WHICH IS WHAT THE PLAN ASKED FOR.

The plan says "diff the fills", presuming a key that pairs a lane-A fill with a
lane-B fill. There is no such key, for two reasons that are properties of the
system rather than of this script.

  1. LANE B IS NOT REPRODUCIBLE AT THE FILL LEVEL. tools/exchange.cpp polls TCP
     and then replays ONE message, so which historical message a strategy order
     lands after is a wall-clock race between two processes. Two runs of the
     same binaries on the same feed with the same flags gave 617 orders / 880
     fills and 634 / 847. Phase 12.8's artifact shows the same thing at scale:
     chain-A hops pool to n = 12,000 = 10 x 1,200 exactly, while every chain-B
     hop pools to n = 20,868 over those same ten runs -- not divisible by ten.

  2. THE TRIGGER COUNTERS DIFFER. The live strategy quotes every N messages that
     MUTATED its book (strategy.cpp:337); the backtester counts every message it
     saw (backtest.hpp:221). Same stride, different instants, different orders.
     Calibrating the stride until the counts matched would bury that under a
     fitted constant.

So the comparison is distributional. That is a weaker claim than the plan wanted
and it is the true one.

THE CATEGORIES, and how each is recognised automatically rather than by eye:

  structural-no-price-priority
      include/itchbook/replay/split.hpp:84 routes every message except 'E' to
      the state path, so a historical order resting at or through a strategy
      order never consults the matcher. Lane B has NO price-priority fill and NO
      through-print fill AT ALL. Lane A labels every fill it takes with the
      trigger that caused it (SimFill::trigger, sim/event.hpp:85), so this is
      counted, not estimated.

  latency-model-shape
      Lane A applies one constant where lane B has a measured distribution.
      Swept across five orders of magnitude rather than assumed: if fills do not
      move, latency cannot be the explanation for anything.

  queue-approximation
      The spread across lane A's four fill models on identical intent, and where
      lane B falls relative to it.

  inventory-and-marking
      Lane B's strategy sees its own orders on the feed and can quote off them;
      lane A's book is built from the feed alone.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

MAGIC = 0x38324254
FILLREC = "<QQIIIBBH"
MODELS = ["naive", "optimistic", "mbo", "pessimistic"]


def lane_b_trace_fills(path):
    blob = open(path, "rb").read()
    if len(blob) < 8:
        return []
    magic, _ver = struct.unpack_from("<II", blob, 0)
    if magic != MAGIC:
        raise SystemExit("error: %s is not a trace (magic %#x)" % (path, magic))
    off, out = 8, []
    while off + 12 <= len(blob):
        tag = blob[off:off + 4].decode("ascii", "replace")
        n, rec = struct.unpack_from("<II", blob, off + 4)
        off += 12
        if tag == "FILL":
            if rec != struct.calcsize(FILLREC):
                raise SystemExit("error: FILL record is %d bytes, expected %d"
                                 % (rec, struct.calcsize(FILLREC)))
            for i in range(n):
                t6p, t6, ref, ordn, sh, in_book, parked, _p = \
                    struct.unpack_from(FILLREC, blob, off + i * rec)
                out.append({"ref": ref, "ordinal": ordn, "shares": sh,
                            "in_book": bool(in_book), "parked": bool(parked)})
        off += n * rec
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lane-a-dir", required=True,
                    help="directory of lane-a-lat*.json from phase12-9-lane-a.sh")
    ap.add_argument("--lane-b-json", required=True)
    ap.add_argument("--lane-b-trace", required=True)
    ap.add_argument("--reference-latency-ns", type=int, default=13000,
                    help="the lane-A run used for the headline comparison")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    sweep = {}
    for p in sorted(glob.glob(os.path.join(args.lane_a_dir, "lane-a-lat*.json"))):
        m = re.search(r"lat(\d+)\.json$", p)
        if not m:
            continue
        sweep[int(m.group(1))] = json.load(open(p, encoding="utf-8"))
    if not sweep:
        raise SystemExit("error: no lane-a-lat*.json in %s" % args.lane_a_dir)
    if args.reference_latency_ns not in sweep:
        raise SystemExit("error: no lane A run at %d ns" % args.reference_latency_ns)

    bj = json.load(open(args.lane_b_json, encoding="utf-8"))
    bt = lane_b_trace_fills(args.lane_b_trace)

    b_orders = bj["orders_sent"]
    b_fills = bj["maker_fills_from_feed"]
    b_shares = bj["maker_fill_shares"]
    b_events = bj["feed_messages"]

    ref = sweep[args.reference_latency_ns]["models"]

    print("=== the two lanes, same 150k-message window ===")
    print("  lane B (live)   events %7d  orders %5d  fills %5d  shares %7d"
          % (b_events, b_orders, b_fills, b_shares))
    print("  %-13s %7s %7s %8s %12s" % ("lane A model", "fills", "shares",
                                        "vs B", "fills/order"))
    for k in MODELS:
        v = ref[k]
        print("  %-13s %7d %7d %8.2fx %12s"
              % (k, v["fills"], v["shares"], float(v["fills"]) / b_fills, "-"))

    # ---- latency: swept, not assumed ----------------------------------------
    print()
    print("=== category: latency-model-shape ===")
    print("  %-12s %7s %11s %7s %12s" % ("latency ns", "naive", "optimistic",
                                         "mbo", "pessimistic"))
    for L in sorted(sweep):
        m = sweep[L]["models"]
        print("  %-12d %7d %11d %7d %12d"
              % (L, m["naive"]["fills"], m["optimistic"]["fills"],
                 m["mbo"]["fills"], m["pessimistic"]["fills"]))
    invariant = all(sweep[L]["models"][k]["fills"]
                    == sweep[sorted(sweep)[0]]["models"][k]["fills"]
                    for L in sweep for k in MODELS)
    if invariant:
        print("  INVARIANT across %d..%d ns. The lanes' latency models differ and"
              % (min(sweep), max(sweep)))
        print("  it CANNOT explain any disagreement below: at this quoting rate a")
        print("  millisecond is far under the interval between quotes.")
    else:
        print("  Fills move with latency; the mismatch between lanes is live.")

    # ---- structural ----------------------------------------------------------
    print()
    print("=== category: structural-no-price-priority ===")
    print("  split.hpp:84 sends everything except 'E' to the state path, so lane B")
    print("  has no price-priority and no through-print fill at all.")
    print("  %-13s %7s %7s %8s %11s %14s" % ("lane A model", "fills", "lock",
                                             "through", "structural", "A minus those"))
    structural = {}
    for k in MODELS:
        v = ref[k]
        s = v["lock_fills"] + v["through_fills"]
        rest = v["fills"] - s
        structural[k] = {"lock": v["lock_fills"], "through": v["through_fills"],
                         "structural": s,
                         "share": (float(s) / v["fills"]) if v["fills"] else 0.0,
                         "fills_without": rest}
        print("  %-13s %7d %7d %8d %10.1f%% %14d"
              % (k, v["fills"], v["lock_fills"], v["through_fills"],
                 100.0 * s / v["fills"], rest))
    print("  lane B observed %d." % b_fills)

    # ---- queue approximation -------------------------------------------------
    print()
    print("=== category: queue-approximation ===")
    lo = min(ref[k]["fills"] for k in MODELS)
    hi = max(ref[k]["fills"] for k in MODELS)
    inside = lo <= b_fills <= hi
    lo2 = min(structural[k]["fills_without"] for k in MODELS)
    hi2 = max(structural[k]["fills_without"] for k in MODELS)
    inside2 = lo2 <= b_fills <= hi2
    print("  band over four models, as reported:      [%d, %d]  lane B %d -> %s"
          % (lo, hi, b_fills, "INSIDE" if inside else "OUTSIDE"))
    print("  band with the structural paths removed:  [%d, %d]  lane B %d -> %s"
          % (lo2, hi2, b_fills, "INSIDE" if inside2 else "OUTSIDE"))

    # ---- inventory -----------------------------------------------------------
    print()
    print("=== category: inventory-and-marking ===")
    print("  lane B saw its own Add on the feed for %d of %d orders; lane A's book"
          % (bj["own_adds_seen_on_feed"], b_orders))
    print("  is built from the feed alone and never contains its own orders.")
    parked = sum(1 for x in bt if x["parked"])
    print("  lane B fills recognised before their acknowledgement: %d of %d"
          % (parked, len(bt)))

    doc = {
        "what": "phase 12.9 replay-vs-live A/B",
        "window_messages": b_events,
        "reference_latency_ns": args.reference_latency_ns,
        "lane_b": {"orders": b_orders, "fills": b_fills, "shares": b_shares,
                   "own_adds_seen_on_feed": bj["own_adds_seen_on_feed"],
                   "parked_fills": parked, "trace_fills": len(bt)},
        "lane_a": {k: {"fills": ref[k]["fills"], "shares": ref[k]["shares"]}
                   for k in MODELS},
        "categories": {
            "latency_model_shape": {
                "swept_ns": sorted(sweep),
                "fills_by_latency": {str(L): {k: sweep[L]["models"][k]["fills"]
                                              for k in MODELS} for L in sorted(sweep)},
                "invariant": invariant,
            },
            "structural_no_price_priority": structural,
            "queue_approximation": {
                "band": [lo, hi], "lane_b": b_fills, "inside_band": inside,
                "band_without_structural": [lo2, hi2],
                "inside_band_without_structural": inside2,
            },
            "inventory_and_marking": {
                "own_adds_seen_on_feed": bj["own_adds_seen_on_feed"],
                "orders": b_orders, "parked_fills": parked,
            },
        },
        "caveats": [
            "Neither lane models market impact: the strategy's orders consume "
            "liquidity the historical participants never saw, and under the 12.0 "
            "hybrid those participants never react to it.",
            "There is no per-fill join key. Lane B's fill count varies run to run "
            "-- two runs of the same binaries on the same feed gave 617 orders / "
            "880 fills and 634 / 847 -- so the comparison is distributional.",
            "The trigger counters differ: lane B counts book-mutating messages, "
            "lane A counts every message, so the same stride puts quotes at "
            "different instants.",
        ],
    }
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2)
        print()
        print("wrote %s" % args.json_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
