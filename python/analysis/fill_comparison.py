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

  * P&L per share is ONE measure across four categories, so every bar is ONE
    colour. Colouring each bar differently would imply the models are four
    series, and reads as decoration.
  * Per SHARE, never total. Totals reward whichever model fills most, which is
    exactly the difference under study, so a total would fold the answer into
    the axis.
  * Every bar is directly labelled. Two of the palette's hues sit under 3:1
    against the light surface, and the rule for that is visible labels or a
    table view; this ships both.
"""
import argparse
import json
import sys
from pathlib import Path

MODEL_ORDER = ["naive", "optimistic", "mbo", "pessimistic"]

# The validated categorical palette, slots 1-4 (light / dark). Only the line
# chart uses more than slot 1: a bar chart of one measure is one series.
SERIES = [("#2a78d6", "#3987e5"), ("#eb6834", "#d95926"),
          ("#1baf7a", "#199e70"), ("#eda100", "#c98500")]

STYLE = """
  .surface { fill: #fcfcfb; }
  .ink     { fill: #0b0b0b; }
  .muted   { fill: #52514e; }
  .grid    { stroke: #d8d7d2; stroke-width: 1; }
  .axis    { stroke: #52514e; stroke-width: 1; }
  .s1 { fill: #2a78d6; stroke: #2a78d6; }
  .s2 { fill: #eb6834; stroke: #eb6834; }
  .s3 { fill: #1baf7a; stroke: #1baf7a; }
  .s4 { fill: #eda100; stroke: #eda100; }
  @media (prefers-color-scheme: dark) {
    .surface { fill: #1a1a19; }
    .ink     { fill: #ffffff; }
    .muted   { fill: #c3c2b7; }
    .grid    { stroke: #3a3a38; }
    .axis    { stroke: #c3c2b7; }
    .s1 { fill: #3987e5; stroke: #3987e5; }
    .s2 { fill: #d95926; stroke: #d95926; }
    .s3 { fill: #199e70; stroke: #199e70; }
    .s4 { fill: #c98500; stroke: #c98500; }
  }
"""


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def svg_bars(labels, values, title, subtitle, unit="c/share"):
    """One measure across categories: horizontal bars, one colour, all labelled."""
    W, H = 720, 90 + 46 * len(labels)
    left, right = 130, 90
    plot_w = W - left - right
    lo = min(0.0, min(values)) if values else 0.0
    hi = max(0.0, max(values)) if values else 1.0
    span = (hi - lo) or 1.0
    pad = span * 0.12
    lo, hi = lo - pad, hi + pad
    span = hi - lo

    def x(v):
        return left + (v - lo) / span * plot_w

    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img" aria-label="{esc(title)}">',
         f"<style>{STYLE}</style>",
         f'<rect class="surface" width="{W}" height="{H}"/>',
         f'<text class="ink" x="16" y="26" font-family="system-ui,sans-serif" '
         f'font-size="15" font-weight="600">{esc(title)}</text>',
         f'<text class="muted" x="16" y="46" font-family="system-ui,sans-serif" '
         f'font-size="11.5">{esc(subtitle)}</text>']

    zero = x(0.0)
    top = 62
    bar_h, gap = 26, 20   # a 2px+ surface gap between adjacent bars
    for i, (lab, v) in enumerate(zip(labels, values)):
        y = top + i * (bar_h + gap)
        x0, x1 = (zero, x(v)) if v >= 0 else (x(v), zero)
        w = max(abs(x1 - x0), 1.5)
        # 4px rounded data-end, square against the zero baseline.
        p.append(f'<rect class="s1" x="{x0:.1f}" y="{y}" width="{w:.1f}" '
                 f'height="{bar_h}" rx="4"/>')
        if v >= 0:
            p.append(f'<rect class="s1" x="{zero:.1f}" y="{y}" width="4" height="{bar_h}"/>')
        else:
            p.append(f'<rect class="s1" x="{zero - 4:.1f}" y="{y}" width="4" height="{bar_h}"/>')
        p.append(f'<text class="ink" x="{left - 12}" y="{y + bar_h * 0.68:.0f}" '
                 f'text-anchor="end" font-family="system-ui,sans-serif" '
                 f'font-size="12.5">{esc(lab)}</text>')
        tx = (x1 + 8) if v >= 0 else (x0 - 8)
        anchor = "start" if v >= 0 else "end"
        p.append(f'<text class="ink" x="{tx:.1f}" y="{y + bar_h * 0.68:.0f}" '
                 f'text-anchor="{anchor}" font-family="ui-monospace,monospace" '
                 f'font-size="12">{v:+.4f}</text>')

    p.append(f'<line class="axis" x1="{zero:.1f}" y1="{top - 8}" x2="{zero:.1f}" '
             f'y2="{top + len(labels) * (bar_h + gap) - gap + 6}"/>')
    p.append(f'<text class="muted" x="{zero:.1f}" y="{H - 12}" text-anchor="middle" '
             f'font-family="system-ui,sans-serif" font-size="11">0 {esc(unit)}</text>')
    p.append("</svg>")
    return "\n".join(p)


def svg_lines(xs, series, title, subtitle, x_label, y_label):
    """Change over a continuous x, one line per model — here colour IS identity."""
    W, H = 760, 420
    left, right, top, bottom = 76, 130, 66, 58
    plot_w, plot_h = W - left - right, H - top - bottom
    all_y = [v for _, vals in series for v in vals]
    lo, hi = min(all_y + [0.0]), max(all_y + [0.0])
    span = (hi - lo) or 1.0
    lo, hi = lo - span * 0.12, hi + span * 0.12
    span = hi - lo
    n = len(xs)

    def px(i):
        return left + (i / max(n - 1, 1)) * plot_w

    def py(v):
        return top + plot_h - (v - lo) / span * plot_h

    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img" aria-label="{esc(title)}">',
         f"<style>{STYLE}</style>",
         f'<rect class="surface" width="{W}" height="{H}"/>',
         f'<text class="ink" x="16" y="26" font-family="system-ui,sans-serif" '
         f'font-size="15" font-weight="600">{esc(title)}</text>',
         f'<text class="muted" x="16" y="46" font-family="system-ui,sans-serif" '
         f'font-size="11.5">{esc(subtitle)}</text>']

    for k in range(5):
        v = lo + span * k / 4
        y = py(v)
        p.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}"/>')
        p.append(f'<text class="muted" x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" '
                 f'font-family="ui-monospace,monospace" font-size="10.5">{v:.3f}</text>')
    for i, xv in enumerate(xs):
        p.append(f'<text class="muted" x="{px(i):.1f}" y="{top + plot_h + 20}" '
                 f'text-anchor="middle" font-family="ui-monospace,monospace" '
                 f'font-size="10.5">{esc(xv)}</text>')
    p.append(f'<text class="muted" x="{left + plot_w / 2:.0f}" y="{H - 16}" '
             f'text-anchor="middle" font-family="system-ui,sans-serif" '
             f'font-size="11">{esc(x_label)}</text>')
    p.append(f'<text class="muted" x="14" y="{top - 12}" '
             f'font-family="system-ui,sans-serif" font-size="11">{esc(y_label)}</text>')

    for si, (name, vals) in enumerate(series):
        cls = f"s{si + 1}"
        pts = " ".join(f"{px(i):.1f},{py(v):.1f}" for i, v in enumerate(vals))
        p.append(f'<polyline class="{cls}" fill="none" stroke-width="2" '
                 f'stroke-linejoin="round" points="{pts}"/>')
        for i, v in enumerate(vals):
            p.append(f'<circle class="{cls}" cx="{px(i):.1f}" cy="{py(v):.1f}" r="4"/>')
        # Direct label at the line end: identity is never colour alone.
        p.append(f'<text class="ink" x="{left + plot_w + 10}" y="{py(vals[-1]) + 4:.1f}" '
                 f'font-family="system-ui,sans-serif" font-size="11.5">{esc(name)}</text>')
    p.append("</svg>")
    return "\n".join(p)


def ascii_table(rows, title):
    out = [title, ""]
    out.append(f"{'model':<14}{'fills':>8}{'shares':>10}{'c/share':>12}"
               f"{'edge c/sh':>12}{'fees c/sh':>12}")
    out.append("-" * 68)
    width = 34
    vals = [r["per_share"] for r in rows]
    scale = max((abs(v) for v in vals), default=1.0) or 1.0
    for r in rows:
        out.append(f"{r['model']:<14}{r['fills']:>8}{r['shares']:>10}"
                   f"{r['per_share']:>12.4f}{r['edge']:>12.4f}{r['fees']:>12.4f}")
    out.append("")
    out.append("P&L per share")
    for r in rows:
        n = int(round(abs(r["per_share"]) / scale * width))
        bar = ("#" * n) if r["per_share"] >= 0 else ("-" * n)
        out.append(f"  {r['model']:<13}|{bar}")
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
            "shares": m["shares"],
            "per_share": m["per_share_micros"] / 10000.0,
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
    ap.add_argument("--no-ascii", action="store_true")
    a = ap.parse_args()

    d, rows = load(a.results)
    if not rows:
        raise SystemExit("error: no model results in that file")

    title = f"P&L per share by fill model — {d.get('strategy', '?')}"
    sub = (f"{d.get('events', 0):,} events. The gap between naive and pessimistic "
           f"is the queue assumption; mbo should sit inside it.")

    if not a.no_ascii:
        print(ascii_table(rows, title))

    if a.svg:
        Path(a.svg).parent.mkdir(parents=True, exist_ok=True)
        Path(a.svg).write_text(
            svg_bars([r["model"] for r in rows], [r["per_share"] for r in rows],
                     title, sub))
        print(f"\nwrote {a.svg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
