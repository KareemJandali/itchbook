#!/usr/bin/env python3
"""phase12-8-figures.py — the two phase 12.8 figures, from the artifact.

WHY THESE TWO AND NOT A BAR CHART OF PERCENTILES. Percentiles do not decompose:
the p99 of a sum is not the sum of the p99s, so a stacked bar built from per-hop
p99s draws an order that never existed. Both figures here are built from single
orders and from conditional means, which are the two things that survive.

  1. tick-to-trade-ranks.svg -- what ONE order at the p50 rank and ONE at the
     p99 rank actually spent its time on. Same chain, two ranks, so the reader
     can see WHERE the tail comes from rather than being told.
  2. tick-to-trade-tail.svg -- the tail-conditional mean E[X | X >= q]. A p99
     says where the boundary is; it says nothing about how far past it the
     orders that cross it land. This does.

Both read validation/tick-to-trade-baremetal.json and nothing else. `--check`
regenerates and diffs, so a stale figure fails in verify-local.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "validation", "tick-to-trade-baremetal.json")
FIGDIR = os.path.join(ROOT, "docs", "figures")
RANKS = os.path.join(FIGDIR, "tick-to-trade-ranks.svg")
TAIL = os.path.join(FIGDIR, "tick-to-trade-tail.svg")

HOPS = ["t0->t1", "t1->t1'", "t1'->t2", "t2->t3"]
LABEL = {"t0->t1": "arrival to applied",
         "t1->t1'": "post-trigger drain",
         "t1'->t2": "decision",
         "t2->t3": "encode, frame, write"}
CLS = {"t0->t1": "s1", "t1->t1'": "s2", "t1'->t2": "s3", "t2->t3": "s4"}

STYLE = """<style>
  .surface { fill: #fcfcfb; }
  .ink     { fill: #0b0b0b; }
  .muted   { fill: #52514e; }
  .on-fill { fill: #fcfcfb; }
  .grid    { stroke: #d8d7d2; stroke-width: 1; }
  .axis    { stroke: #52514e; stroke-width: 1; }
  .s1 { fill: #2a78d6; stroke: #2a78d6; }
  .s2 { fill: #eb6834; stroke: #eb6834; }
  .s3 { fill: #1baf7a; stroke: #1baf7a; }
  .s4 { fill: #eda100; stroke: #eda100; }
  .k1 { fill: none; stroke: #2a78d6; stroke-width: 2; }
  @media (prefers-color-scheme: dark) {
    .surface { fill: #1a1a19; }
    .ink     { fill: #ffffff; }
    .muted   { fill: #c3c2b7; }
    .on-fill { fill: #1a1a19; }
    .grid    { stroke: #3a3a38; }
    .axis    { stroke: #c3c2b7; }
    .s1 { fill: #3987e5; stroke: #3987e5; }
    .s2 { fill: #d95926; stroke: #d95926; }
    .s3 { fill: #199e70; stroke: #199e70; }
    .s4 { fill: #d59200; stroke: #d59200; }
    .k1 { fill: none; stroke: #3987e5; stroke-width: 2; }
  }
  text { font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI",
         Helvetica, Arial, sans-serif; }
</style>"""


def esc(t):
    return (str(t).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def load():
    art = json.load(open(ART, encoding="utf-8"))
    per_run = art.get("per_run", [])
    ranks, tails = {}, {}
    for label in ("p50", "p99"):
        rows = [r["rank_decomposition"][label] for r in per_run
                if r.get("rank_decomposition", {}).get(label)]
        if not rows:
            continue
        ranks[label] = {
            "runs": len(rows),
            "headline": sum(x["headline_ns"] for x in rows) // len(rows),
            "hops": {h: sum(x["hops_ns"].get(h, 0) for x in rows) // len(rows)
                     for h in HOPS},
        }
    for r in per_run:
        for e in r.get("tail_conditional_mean", []):
            tails.setdefault(e["threshold_pct"], []).append(e)
    tail = []
    for q in sorted(tails):
        g = tails[q]
        tail.append({
            "threshold_pct": q,
            "threshold_ns": sum(e["threshold_ns"] for e in g) // len(g),
            "mean_ns": sum(e["mean_ns"] for e in g) // len(g),
            "n_at_or_above": sum(e["n_at_or_above"] for e in g),
        })
    return art, ranks, tail


# ---- figure 1: two orders, decomposed -----------------------------------------
def fig_ranks(ranks):
    W, H = 720, 340
    x0, x1 = 150, 660
    top = 78
    bh, gap = 62, 74
    peak = max(sum(v["hops"].values()) for v in ranks.values())
    scale = (x1 - x0) / float(peak)

    L = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
         'height="%d" role="img" aria-label="One order at the p50 rank and one at '
         'the p99 rank, decomposed by hop">' % (W, H, W, H), STYLE,
         '<rect class="surface" x="0" y="0" width="%d" height="%d"/>' % (W, H),
         '<text class="ink" x="20" y="30" font-size="16" font-weight="600">'
         'Where a slow order loses its time</text>',
         '<text class="muted" x="20" y="50" font-size="12">One order at the p50 '
         'rank and one at the p99 rank, arrival to write. Mean over %d runs; '
         'percentiles do not decompose, single orders do.</text>'
         % list(ranks.values())[0]["runs"]]

    for i, (label, v) in enumerate(sorted(ranks.items())):
        y = top + i * (bh + gap)
        total = sum(v["hops"].values())
        L.append('<text class="ink" x="20" y="%d" font-size="13" '
                 'font-weight="600">%s rank</text>' % (y + 24, label))
        L.append('<text class="muted" x="20" y="%d" font-size="11">%s ns</text>'
                 % (y + 42, "{:,}".format(total)))
        x = x0
        for h in HOPS:
            w = v["hops"][h] * scale
            if w <= 0:
                continue
            L.append('<rect class="%s" x="%.1f" y="%d" width="%.1f" height="%d"/>'
                     % (CLS[h], x, y, w, bh))
            if w > 46:
                L.append('<text class="on-fill" x="%.1f" y="%d" font-size="11" '
                         'text-anchor="middle">%s</text>'
                         % (x + w / 2, y + bh / 2 + 4,
                            "{:,}".format(v["hops"][h])))
            x += w
        # the headline is the last two segments, marked rather than described
        hx = x0 + (v["hops"]["t0->t1"] + v["hops"]["t1->t1'"]) * scale
        L.append('<line class="axis" x1="%.1f" y1="%d" x2="%.1f" y2="%d" '
                 'stroke-dasharray="3 3"/>' % (hx, y - 8, hx, y + bh + 16))
        L.append('<text class="muted" x="%.1f" y="%d" font-size="10">'
                 '&#8592; drain &#183; headline t1\'&#8594;t3 &#8594;</text>'
                 % (hx - 84, y + bh + 28))

    lx = x0
    for h in HOPS:
        L.append('<rect class="%s" x="%d" y="%d" width="11" height="11"/>'
                 % (CLS[h], lx, H - 26))
        L.append('<text class="muted" x="%d" y="%d" font-size="11">%s</text>'
                 % (lx + 16, H - 16, esc(LABEL[h])))
        lx += 130
    L.append("</svg>")
    return "\n".join(L) + "\n"


# ---- figure 2: how far past the threshold ------------------------------------
def fig_tail(tail):
    W, H = 720, 380
    x0, x1, y0, y1 = 90, 680, 300, 80
    peak = max(e["mean_ns"] for e in tail) * 1.08
    n = len(tail)

    def px(i):
        return x0 + (x1 - x0) * (i / float(max(1, n - 1)))

    def py(v):
        return y0 - (y0 - y1) * (v / peak)

    L = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
         'height="%d" role="img" aria-label="Tail-conditional mean of the '
         'tick-to-trade headline">' % (W, H, W, H), STYLE,
         '<rect class="surface" x="0" y="0" width="%d" height="%d"/>' % (W, H),
         '<text class="ink" x="20" y="30" font-size="16" font-weight="600">'
         'How far past the threshold the slow orders land</text>',
         '<text class="muted" x="20" y="50" font-size="12">Tail-conditional mean '
         'E[X | X &#8805; q] of the headline t1\'&#8594;t3. A percentile says '
         'where the boundary is, not what is beyond it.</text>']

    for f in (0.0, 0.25, 0.5, 0.75, 1.0):
        v = peak * f
        L.append('<line class="grid" x1="%d" y1="%.1f" x2="%d" y2="%.1f"/>'
                 % (x0, py(v), x1, py(v)))
        L.append('<text class="muted" x="%d" y="%.1f" font-size="10" '
                 'text-anchor="end">%s</text>' % (x0 - 8, py(v) + 3,
                                                  "{:,}".format(int(v))))
    L.append('<text class="muted" x="%d" y="%d" font-size="10" '
             'text-anchor="end">ns</text>' % (x0 - 8, y1 - 12))
    L.append('<line class="axis" x1="%d" y1="%d" x2="%d" y2="%d"/>'
             % (x0, y0, x1, y0))

    pts = " ".join("%.1f,%.1f" % (px(i), py(e["mean_ns"]))
                   for i, e in enumerate(tail))
    L.append('<polyline class="k1" points="%s"/>' % pts)
    for i, e in enumerate(tail):
        L.append('<circle class="s1" cx="%.1f" cy="%.1f" r="4"/>'
                 % (px(i), py(e["mean_ns"])))
        L.append('<text class="ink" x="%.1f" y="%.1f" font-size="11" '
                 'text-anchor="middle">%s</text>'
                 % (px(i), py(e["mean_ns"]) - 12,
                    "{:,}".format(e["mean_ns"])))
        q = e["threshold_pct"]
        lab = "all" if q == 0 else ("p%g" % q)
        L.append('<text class="muted" x="%.1f" y="%d" font-size="11" '
                 'text-anchor="middle">%s</text>' % (px(i), y0 + 18, lab))
        L.append('<text class="muted" x="%.1f" y="%d" font-size="10" '
                 'text-anchor="middle">&#8805; %s ns</text>'
                 % (px(i), y0 + 34, "{:,}".format(e["threshold_ns"])))
        L.append('<text class="muted" x="%.1f" y="%d" font-size="10" '
                 'text-anchor="middle">n=%s</text>'
                 % (px(i), y0 + 50, "{:,}".format(e["n_at_or_above"])))
    L.append('<text class="muted" x="%d" y="%d" font-size="11" '
             'text-anchor="middle">threshold q</text>'
             % ((x0 + x1) / 2, y0 + 74))
    L.append("</svg>")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    art, ranks, tail = load()
    if not ranks or not tail:
        print("phase12-8-figures: the artifact carries no figure inputs")
        return 1
    want = {RANKS: fig_ranks(ranks), TAIL: fig_tail(tail)}
    if args.check:
        for p, text in want.items():
            if not os.path.exists(p):
                print("phase12-8-figures: %s does not exist" % p)
                return 1
            if open(p, encoding="utf-8").read() != text:
                print("phase12-8-figures: %s does not match the artifact; "
                      "re-run scripts/phase12-8-figures.py" % os.path.basename(p))
                return 1
        print("phase12-8 figures match their artifact")
        return 0
    for p, text in want.items():
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
