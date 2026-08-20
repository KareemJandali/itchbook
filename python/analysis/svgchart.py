#!/usr/bin/env python3
"""Dependency-free SVG primitives shared by the phase 6 charts.

The repo has no plotting dependency and is not acquiring one to draw a bar
chart. Two forms live here and the split between them is the whole point:

  * `svg_bars` — ONE measure across categories. Every bar is one colour,
    because four colours would claim the categories are four series and read
    as decoration.
  * `svg_lines` — change over a continuous x, one line per model. Here colour
    IS identity, so the four hues are the point, and every line is also
    directly labelled at its end so identity is never colour alone.

The palette is the validated four-slot categorical set, light and dark steps
chosen against their own surface rather than flipped. Two of the light-mode
hues sit under 3:1 against the surface; the rule for that is visible labels or
a table view, and every caller here ships both.
"""


# The validated categorical palette, slots 1-4 (light / dark). Only the line
# chart uses more than slot 1: a bar chart of one measure is one series.
SERIES = [("#2a78d6", "#3987e5"), ("#eb6834", "#d95926"),
          ("#1baf7a", "#199e70"), ("#eda100", "#c98500")]

STYLE = """
  .surface { fill: #fcfcfb; }
  .ink     { fill: #0b0b0b; }
  .muted   { fill: #52514e; }
  /* Text drawn ON a filled mark, so it needs the surface colour, not ink. */
  .on-fill { fill: #fcfcfb; }
  .grid    { stroke: #d8d7d2; stroke-width: 1; }
  .axis    { stroke: #52514e; stroke-width: 1; }
  .s1 { fill: #2a78d6; stroke: #2a78d6; }
  .s2 { fill: #eb6834; stroke: #eb6834; }
  .s3 { fill: #1baf7a; stroke: #1baf7a; }
  .s4 { fill: #eda100; stroke: #eda100; }
  /* Stroke-only twins. A class that sets `fill` beats a fill="none"
     presentation attribute in the cascade, which silently turns every
     polyline into a filled blob. */
  .k1 { fill: none; stroke: #2a78d6; }
  .k2 { fill: none; stroke: #eb6834; }
  .k3 { fill: none; stroke: #1baf7a; }
  .k4 { fill: none; stroke: #eda100; }
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
    .s4 { fill: #c98500; stroke: #c98500; }
    .k1 { stroke: #3987e5; }
    .k2 { stroke: #d95926; }
    .k3 { stroke: #199e70; }
    .k4 { stroke: #c98500; }
  }
"""


def fmt_for(values):
    """Pick a number format from the data's magnitude and spread.

    A share count rendered as `140583.000` is not a rounding nit: it tells the
    reader the quantity is continuous and measured to a thousandth, which is
    false. Cents per share genuinely need four decimals, because the whole
    subject is differences of a hundredth of a cent.
    """
    span = (max(values) - min(values)) if values else 0.0
    scale = max((abs(v) for v in values), default=0.0)
    if all(float(v).is_integer() for v in values) and scale >= 1000:
        return lambda v: f"{v:,.0f}"
    if scale >= 100:
        return lambda v: f"{v:,.1f}"
    if span and span < 0.05:
        return lambda v: f"{v:.4f}"
    return lambda v: f"{v:.3f}"


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def svg_bars(labels, values, title, subtitle, unit="c/share", value_fmt=None):
    """One measure across categories: horizontal bars, one colour, all labelled."""
    W, H = 720, 90 + 46 * len(labels)
    left, right = 130, 90
    plot_w = W - left - right
    show = value_fmt or (lambda v: f"{v:+.4f}")
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
        # A negative bar grows LEFTWARD from zero, straight at the category
        # labels, so its value label cannot live outside it: on the first
        # all-negative chart this code drew, the longest bar rendered as
        # "mbo3,425.4" with the minus sign hidden behind the category name —
        # the largest loss reading as a gain. Long negative bars carry their
        # label inside, right-aligned at the zero end, where there is always
        # room and nothing to collide with.
        label = show(v)
        inside = v < 0 and w > 8 * len(label) + 16
        if inside:
            tx, anchor, cls = zero - 10, "end", "on-fill"
        elif v >= 0:
            tx, anchor, cls = x1 + 8, "start", "ink"
        else:
            tx, anchor, cls = x0 - 8, "end", "ink"
        p.append(f'<text class="{cls}" x="{tx:.1f}" y="{y + bar_h * 0.68:.0f}" '
                 f'text-anchor="{anchor}" font-family="ui-monospace,monospace" '
                 f'font-size="12">{esc(label)}</text>')

    p.append(f'<line class="axis" x1="{zero:.1f}" y1="{top - 8}" x2="{zero:.1f}" '
             f'y2="{top + len(labels) * (bar_h + gap) - gap + 6}"/>')
    p.append(f'<text class="muted" x="{zero:.1f}" y="{H - 12}" text-anchor="middle" '
             f'font-family="system-ui,sans-serif" font-size="11">0 {esc(unit)}</text>')
    p.append("</svg>")
    return "\n".join(p)


def svg_lines(xs, series, title, subtitle, x_label, y_label, include_zero=False):
    """Change over a continuous x, one line per model — here colour IS identity.

    Two deliberate departures from the bar chart:

      * **The y axis is not forced through zero, unless the caller says the
        sign is the question.** A bar encodes magnitude by length and must
        start at zero or it lies; a line encodes change, and padding a
        1.5-to-1.9 range down to zero throws away nine tenths of the plot to
        show an axis nobody was asking about. But a markout is read by its
        SIGN — picked off or not — and a chart of it that crops zero off the
        bottom makes a small positive number look like a large one. Pass
        `include_zero` there. When zero still falls outside the range the axis
        label says so, so a truncated axis is never a truncated axis the
        reader had to notice for themselves.
      * **End labels are pushed apart.** Four lines that converge would stack
        four labels on one pixel row. They are separated by the minimum that
        keeps them legible, in the drawing only — the lines and markers stay
        exactly where the data puts them.
    """
    W, H = 780, 430
    left, right, top, bottom = 78, 132, 78, 62
    plot_w, plot_h = W - left - right, H - top - bottom
    all_y = [v for _, vals in series for v in vals]
    lo, hi = min(all_y), max(all_y)
    if include_zero:
        lo, hi = min(lo, 0.0), max(hi, 0.0)
    if lo == hi:
        lo, hi = lo - 1.0, hi + 1.0
    span = hi - lo
    lo, hi = lo - span * 0.12, hi + span * 0.12
    span = hi - lo
    n = len(xs)
    zero_shown = lo <= 0.0 <= hi

    def px(i):
        return left + (i / max(n - 1, 1)) * plot_w

    def py(v):
        return top + plot_h - (v - lo) / span * plot_h

    axis_note = y_label if zero_shown else f"{y_label} (axis excludes zero)"
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img" aria-label="{esc(title)}">',
         f"<style>{STYLE}</style>",
         f'<rect class="surface" width="{W}" height="{H}"/>',
         f'<text class="ink" x="16" y="26" font-family="system-ui,sans-serif" '
         f'font-size="15" font-weight="600">{esc(title)}</text>',
         f'<text class="muted" x="16" y="46" font-family="system-ui,sans-serif" '
         f'font-size="11.5">{esc(subtitle)}</text>',
         f'<text class="muted" x="16" y="66" font-family="system-ui,sans-serif" '
         f'font-size="11">{esc(axis_note)}</text>']

    # Recessive grid, five steps. The zero line, when it is in range, is drawn
    # on the axis stroke instead: it is the one gridline that means something.
    # Decided from the DATA, not from the interpolated tick values: padding
    # a whole-number series produces fractional ticks, and letting those
    # vote turns a share count back into `140,583.0`.
    tick = fmt_for(all_y)
    for k in range(5):
        v = lo + span * k / 4
        y = py(v)
        p.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}"/>')
        p.append(f'<text class="muted" x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" '
                 f'font-family="ui-monospace,monospace" font-size="10.5">{esc(tick(v))}</text>')
    if zero_shown:
        # The one gridline that means something, so it gets the axis stroke and
        # its own label rather than being left as an unexplained dark rule.
        p.append(f'<line class="axis" x1="{left}" y1="{py(0.0):.1f}" '
                 f'x2="{left + plot_w}" y2="{py(0.0):.1f}"/>')
        p.append(f'<text class="ink" x="{left - 10}" y="{py(0.0) + 4:.1f}" '
                 f'text-anchor="end" font-family="ui-monospace,monospace" '
                 f'font-size="10.5">0</text>')

    for i, xv in enumerate(xs):
        p.append(f'<text class="muted" x="{px(i):.1f}" y="{top + plot_h + 20}" '
                 f'text-anchor="middle" font-family="ui-monospace,monospace" '
                 f'font-size="10.5">{esc(xv)}</text>')
    p.append(f'<text class="muted" x="{left + plot_w / 2:.0f}" y="{top + plot_h + 44}" '
             f'text-anchor="middle" font-family="system-ui,sans-serif" '
             f'font-size="11">{esc(x_label)}</text>')

    for si, (name, vals) in enumerate(series):
        cls = si % 4 + 1
        pts = " ".join(f"{px(i):.1f},{py(v):.1f}" for i, v in enumerate(vals))
        p.append(f'<polyline class="k{cls}" stroke-width="2" '
                 f'stroke-linejoin="round" points="{pts}"/>')
        for i, v in enumerate(vals):
            p.append(f'<circle class="s{cls}" cx="{px(i):.1f}" cy="{py(v):.1f}" r="4"/>')

    # Direct labels at the line ends, nudged apart top-down so converging lines
    # do not stack four labels on one row. Identity is never colour alone.
    ends = sorted(((py(vals[-1]), name, si) for si, (name, vals) in enumerate(series)))
    placed = []
    for y, name, si in ends:
        y = max(y, (placed[-1] + 15) if placed else top + 6)
        placed.append(y)
        cls = si % 4 + 1
        p.append(f'<circle class="s{cls}" cx="{left + plot_w + 12}" cy="{y - 4:.1f}" r="4"/>')
        p.append(f'<text class="ink" x="{left + plot_w + 22}" y="{y:.1f}" '
                 f'font-family="system-ui,sans-serif" font-size="11.5">{esc(name)}</text>')
    p.append("</svg>")
    return "\n".join(p)
