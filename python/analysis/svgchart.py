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


def svg_loglog(xs, series, title, subtitle, x_label, y_label, rules=()):
    """Latency against offered rate: both axes logarithmic, with vertical rules.

    A third form, because neither of the two above can draw this honestly.

      * `svg_lines` positions x by INDEX. That is right for a handful of named
        configurations and wrong here: the rate ladder is geometric, 1x to
        800x, and index spacing would draw the gap from 1x to 2x the same width
        as the gap from 400x to 800x. Those are the same ratio, so on a log
        axis they SHOULD be equal -- but index spacing also makes 1x-to-2x
        equal to 100x-to-200x while claiming a linear axis, which is a
        different chart telling a different lie depending on the ladder.
      * y must be log for the same reason the histogram's is: p50 sits in the
        microseconds and p99.9 above the knee is in the milliseconds, and on a
        linear axis every percentile below the worst one is a flat line on the
        floor. The interesting claim -- that p50 barely moves while the tail
        explodes -- is exactly what a linear axis erases.

    Both axes are labelled as log, and gridded at decades so the spacing gives
    the scale away without the reader having to take the label on trust.

    `rules` is a sequence of (label, x) drawn as vertical dashed lines: the knee
    and the max sustainable rate. They are the deliverable, so they are drawn on
    the chart rather than left to a caption.
    """
    import math
    W, H = 820, 460
    left, right, top, bottom = 82, 140, 92, 66
    plot_w, plot_h = W - left - right, H - top - bottom

    all_y = [v for _, vals in series for v in vals if v > 0]
    if not all_y or not xs:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"></svg>'
    ylo, yhi = math.log10(min(all_y)), math.log10(max(all_y))
    if yhi - ylo < 0.5:
        mid = (ylo + yhi) / 2
        ylo, yhi = mid - 0.25, mid + 0.25
    ylo, yhi = ylo - 0.08 * (yhi - ylo), yhi + 0.08 * (yhi - ylo)
    xlo, xhi = math.log10(min(xs)), math.log10(max(xs))
    if xhi - xlo < 0.3:
        xlo, xhi = xlo - 0.15, xhi + 0.15
    xlo, xhi = xlo - 0.04 * (xhi - xlo), xhi + 0.04 * (xhi - xlo)

    def px(v):
        return left + (math.log10(v) - xlo) / (xhi - xlo) * plot_w

    def py(v):
        return top + plot_h - (math.log10(max(v, 1e-9)) - ylo) / (yhi - ylo) * plot_h

    def si_fmt(v):
        for div, suf in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
            if v >= div:
                q = v / div
                return f"{q:.0f}{suf}" if q >= 10 else f"{q:.1f}{suf}"
        return f"{v:.0f}"

    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img" aria-label="{esc(title)}">',
         f"<style>{STYLE}</style>",
         f'<rect class="surface" width="{W}" height="{H}"/>',
         f'<text class="ink" x="16" y="26" font-family="system-ui,sans-serif" '
         f'font-size="15" font-weight="600">{esc(title)}</text>',
         f'<text class="muted" x="16" y="46" font-family="system-ui,sans-serif" '
         f'font-size="11.5">{esc(subtitle)}</text>',
         f'<text class="muted" x="16" y="66" font-family="system-ui,sans-serif" '
         f'font-size="11">both axes logarithmic; gridlines at decades</text>']

    d = math.floor(ylo)
    while d <= yhi:
        if d >= ylo:
            y = py(10 ** d)
            p.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" '
                     f'x2="{left + plot_w}" y2="{y:.1f}"/>')
            p.append(f'<text class="muted" x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" '
                     f'font-family="ui-monospace,monospace" font-size="10.5">'
                     f'{esc(si_fmt(10 ** d))}</text>')
        d += 1
    d = math.floor(xlo)
    while d <= xhi:
        if d >= xlo:
            x = px(10 ** d)
            p.append(f'<line class="grid" x1="{x:.1f}" y1="{top}" '
                     f'x2="{x:.1f}" y2="{top + plot_h}"/>')
            p.append(f'<text class="muted" x="{x:.1f}" y="{top + plot_h + 18}" '
                     f'text-anchor="middle" font-family="ui-monospace,monospace" '
                     f'font-size="10.5">{esc(si_fmt(10 ** d))}</text>')
        d += 1

    # The annotations, drawn before the data so the lines sit on top of them.
    for i, (label, xv) in enumerate(rules):
        if xv is None or xv <= 0:
            continue
        x = px(xv)
        p.append(f'<line class="axis" x1="{x:.1f}" y1="{top}" x2="{x:.1f}" '
                 f'y2="{top + plot_h}" stroke-dasharray="5 4"/>')
        p.append(f'<text class="ink" x="{x + 5:.1f}" y="{top + 14 + i * 15:.1f}" '
                 f'font-family="system-ui,sans-serif" font-size="11">{esc(label)}</text>')

    p.append(f'<text class="muted" x="{left + plot_w / 2:.0f}" y="{top + plot_h + 44}" '
             f'text-anchor="middle" font-family="system-ui,sans-serif" '
             f'font-size="11">{esc(x_label)}</text>')
    p.append(f'<text class="muted" transform="translate(20,{top + plot_h / 2:.0f}) rotate(-90)" '
             f'text-anchor="middle" font-family="system-ui,sans-serif" '
             f'font-size="11">{esc(y_label)}</text>')

    for si, (name, vals) in enumerate(series):
        cls = si % 4 + 1
        pts = " ".join(f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, vals) if v > 0)
        p.append(f'<polyline class="k{cls}" stroke-width="2" '
                 f'stroke-linejoin="round" points="{pts}"/>')
        for x, v in zip(xs, vals):
            if v > 0:
                p.append(f'<circle class="s{cls}" cx="{px(x):.1f}" cy="{py(v):.1f}" r="3.5"/>')

    ends = sorted(((py(vals[-1]), name, si) for si, (name, vals) in enumerate(series)
                   if vals and vals[-1] > 0))
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


def svg_histogram(buckets, title, subtitle, markers=(), x_label="cycles per message",
                  y_label="messages", compare=None, labels=("before", "after")):
    """A distribution over log-spaced buckets: vertical bars, one colour.

    `buckets` is a sequence of (lo, hi, count); `markers` a sequence of
    (label, x) drawn as vertical rules.

    Both axes are logarithmic, and that is a claim the chart has to make out
    loud rather than let a reader assume linear — so both are labelled as log
    and gridded at decades, where the spacing itself gives the scale away.
    The reason for each:

      * x, because per-message cycle costs here run from tens to millions. On
        a linear axis the entire distribution is one bar hard against the
        origin and the tail is six screens of white space.
      * y, because the tail is the point. p99.9 lives in buckets holding a few
        hundred samples next to a mode holding tens of thousands; on a linear
        count axis those bars are under a pixel and the chart shows a single
        spike, which is exactly the information the percentile table already
        gave us.

    One measure, so one colour — the bars are not four series. Identity comes
    from the title, and the percentile markers are labelled directly rather
    than through a legend.

    With `compare` (a second bucket list), the form changes and so does the
    colour rule. Two distributions are two series, colour becomes identity,
    and both get a legend AND a direct end-label so identity is never colour
    alone. They are drawn as stepped OUTLINES rather than filled bars: two
    sets of filled bars on a shared log axis occlude each other exactly where
    they differ most, which is the part the reader came for. `compare` is the
    baseline, drawn first and underneath.
    """
    import math

    def clean(bs):
        return [(max(int(lo), 1), int(hi), int(c)) for lo, hi, c in bs if int(c) > 0]

    buckets = clean(buckets)
    if not buckets:
        raise ValueError("no non-empty buckets to draw")
    base = clean(compare) if compare else []
    # Both series share one pair of axes. Scaling each to its own range would
    # draw two distributions that look alike and are not comparable, which is
    # the whole point of putting them together.
    span_src = buckets + base

    W, H = 720, 440

    x_lo = math.log10(min(b[0] for b in span_src))
    x_hi = math.log10(max(b[1] for b in span_src))
    x_lo, x_hi = math.floor(x_lo), math.ceil(x_hi)
    x_span = (x_hi - x_lo) or 1

    y_hi = math.ceil(math.log10(max(b[2] for b in span_src)))
    y_span = y_hi or 1          # y starts at 1 sample = log 0

    # The left margin is whatever the widest y-axis label needs. A fixed 62
    # fits "100,000" and clips "1,000,000", and which one appears depends on
    # how many messages were benchmarked — so the gutter has to be measured,
    # not guessed. 6.4px per glyph at 10.5px monospace, plus the 8px gap.
    top_label = f"{10 ** y_span:,}"
    left = int(22 + 6.4 * len(top_label))
    right, top, bottom = 30, 112, 62
    plot_w, plot_h = W - left - right, H - top - bottom

    def X(cycles):
        return left + (math.log10(max(cycles, 1)) - x_lo) / x_span * plot_w

    def Y(count):
        return top + plot_h - (math.log10(max(count, 1)) / y_span) * plot_h

    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img" aria-label="{esc(title)}">',
         f"<style>{STYLE}</style>",
         f'<rect class="surface" width="{W}" height="{H}"/>',
         f'<text class="ink" x="16" y="26" font-family="system-ui,sans-serif" '
         f'font-size="15" font-weight="600">{esc(title)}</text>',
         f'<text class="muted" x="16" y="46" font-family="system-ui,sans-serif" '
         f'font-size="11.5">{esc(subtitle)}</text>']

    # Recessive decade grid, y first so the bars sit over it.
    for d in range(0, y_span + 1):
        y = top + plot_h - (d / y_span) * plot_h
        p.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{W - right}" y2="{y:.1f}"/>')
        p.append(f'<text class="muted" x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" '
                 f'font-family="ui-monospace,monospace" font-size="10.5">'
                 f'{10 ** d:,}</text>')
    # A decade label is ~7px per glyph, so past six decades the big ones on the
    # right run into each other ("10,000,000" and "100,000,000" overprinted the
    # first time this drew a before/after pair). Label every other decade once
    # the axis is wider than that, keeping the first and last.
    label_every = 1 if (x_hi - x_lo) <= 6 else 2
    for d in range(x_lo, x_hi + 1):
        x = X(10 ** d)
        p.append(f'<line class="grid" x1="{x:.1f}" y1="{top}" x2="{x:.1f}" '
                 f'y2="{top + plot_h}"/>')
        if d != x_lo and d != x_hi and (d - x_lo) % label_every:
            continue
        # The end labels anchor inward. A centred "10,000,000" at the last
        # decade hangs ~20px past the viewBox and is simply cut off, and how
        # far it hangs depends on the data's magnitude — so anchoring, not a
        # wider margin, is the fix that keeps working.
        anchor = "start" if d == x_lo else "end" if d == x_hi else "middle"
        p.append(f'<text class="muted" x="{x:.1f}" y="{top + plot_h + 18}" '
                 f'text-anchor="{anchor}" font-family="ui-monospace,monospace" '
                 f'font-size="10.5">{10 ** d:,}</text>')

    baseline = top + plot_h

    def step_points(bs):
        """A stepped outline through the buckets: up the left edge, across the
        top, and only then down — so the shape traces the distribution rather
        than joining bucket midpoints, which would imply counts between them
        that were never measured."""
        pts = []
        for lo, hi, count in bs:
            y = Y(count)
            pts.append((X(lo), baseline if not pts else pts[-1][1]))
            pts.append((X(lo), y))
            pts.append((X(hi), y))
        if pts:
            pts.append((pts[-1][0], baseline))
        return pts

    if base:
        # Two series: outlines, colour as identity, both labelled.
        for i, (bs, cls) in enumerate(((base, 'k2'), (buckets, 'k1'))):
            pts = step_points(bs)
            d = ' '.join(f'{x:.1f},{y:.1f}' for x, y in pts)
            p.append(f'<polyline class="{cls}" stroke-width="2" points="{d}"/>')
            # Direct label at the series' tallest point, so identity survives
            # a greyscale print or a red-green reader. Two similar
            # distributions peak in the SAME bucket — a before/after pair
            # usually does, that is the point — so the two labels are stacked
            # rather than both placed at the peak, where they overprinted each
            # other into an unreadable smear on the first render.
            peak = max(bs, key=lambda b: b[2])
            lx = X(peak[0])
            ly = Y(peak[2]) - 10 - (1 - i) * 15
            p.append(f'<text class="ink" x="{lx:.1f}" y="{ly:.1f}" text-anchor="middle" '
                     f'font-family="system-ui,sans-serif" font-size="11" '
                     f'font-weight="600">{esc(labels[i])}</text>')
    else:
        # The bars. Each spans its own bucket bounds, less a 2px surface gap, so
        # adjacent bars read as separate marks rather than one filled region.
        for lo, hi, count in buckets:
            x0, x1 = X(lo), X(hi)
            w = max(x1 - x0 - 2, 1.2)
            y = Y(count)
            h = baseline - y
            if h < 1.2:
                h, y = 1.2, baseline - 1.2
            p.append(f'<rect class="s1" x="{x0 + 1:.1f}" y="{y:.1f}" width="{w:.1f}" '
                     f'height="{h:.1f}" rx="2"/>')

    p.append(f'<line class="axis" x1="{left}" y1="{baseline}" x2="{W - right}" '
             f'y2="{baseline}"/>')

    # Percentile markers, labelled directly. These are the tie back to the
    # percentile table: same numbers, shown against the shape they summarise.
    for i, (label, xv) in enumerate(markers):
        x = X(xv)
        p.append(f'<line class="axis" x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" '
                 f'y2="{baseline}" stroke-dasharray="4 3"/>')
        # Stagger so adjacent markers cannot overprint each other, in a band
        # reserved for them below the subtitle and the y-axis label. The first
        # version of this put them at top - 10 with top = 74, which printed
        # "p99 512" straight through the subtitle.
        ly = top - 14 - (i % 2) * 16
        p.append(f'<text class="ink" x="{x:.1f}" y="{ly}" text-anchor="middle" '
                 f'font-family="ui-monospace,monospace" font-size="10.5">'
                 f'{esc(label)} {xv:,}</text>')

    # A legend whenever there are two series, in addition to the direct labels.
    if base:
        lx = left + 4
        for i, cls in enumerate(('s2', 's1')):
            p.append(f'<rect class="{cls}" x="{lx}" y="{H - 22}" width="10" height="10" rx="2"/>')
            p.append(f'<text class="muted" x="{lx + 15}" y="{H - 13}" '
                     f'font-family="system-ui,sans-serif" font-size="11">'
                     f'{esc(labels[i])}</text>')
            lx += 22 + 7 * len(labels[i])

    p.append(f'<text class="muted" x="{left + plot_w / 2:.0f}" y="{H - 26}" '
             f'text-anchor="middle" font-family="system-ui,sans-serif" '
             f'font-size="11">{esc(x_label)} — log scale</text>')
    p.append(f'<text class="muted" x="16" y="66" '
             f'font-family="system-ui,sans-serif" font-size="11">'
             f'{esc(y_label)} — log</text>')
    p.append("</svg>")
    return "\n".join(p)
