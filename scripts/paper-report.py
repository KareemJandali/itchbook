#!/usr/bin/env python3
"""Generate the results sections of docs/paper/as-on-itch.md.

    python3 scripts/paper-report.py           # write them
    python3 scripts/paper-report.py --check   # fail if stale

Standing rule 7 -- no number reaches a document by being retyped -- applied to
the paper itself. Everything between the `generated:` markers comes out of
committed JSON, and CI runs this with --check so prose and artifact cannot
drift apart.

This script adds a second rule that the earlier report generators did not need:
IT REFUSES TO EMIT A RESULTS TABLE IT DOES NOT HAVE. When the evaluation
artifact is missing it writes an explicit "no results" block naming exactly what
is absent, rather than leaving a hole that reads like an oversight or, worse,
leaving last week's numbers in place. A paper whose results section is empty
because the run has not happened should look different from one whose results
section is empty because someone deleted a table.

The seven predictions in docs/build-plan-9-12.md 11.3 are graded HERE, by
computation. Phase 9's report script spent a while cheerfully printing "kept"
after the numbers had moved outside the predicted range, which is the failure
mode that makes a pre-registered prediction worthless. The bars below are
transcribed once from the committed plan; every verdict is derived from them.
"""
import argparse
import glob
import json
import math
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAPER = ROOT / "docs" / "paper" / "as-on-itch.md"
CALIB_GLOB = str(ROOT / "validation" / "intensity*.json")
EXPT = ROOT / "validation" / "as-experiment.json"
EXPT_GLOB = str(ROOT / "validation" / "as-experiment*.json")
PHASE6 = ROOT / "docs" / "figures" / "touch-maker.json"
REVIEW = ROOT / "docs" / "paper" / "review" / "review.json"

FIGREL = "../figures/paper"
FIGDIR = ROOT / "docs" / "figures" / "paper"
MANIFEST = FIGDIR / "manifest.json"

MODELS = ["naive", "optimistic", "mbo", "pessimistic"]
ARMS = ["symmetric-touch", "as-gamma0", "as"]

# The pre-registered scope, from the Phase 11 done-list in the plan.
MIN_SYMBOLS = 3
MIN_EVAL_DAYS = 3

# And its COMPOSITION, which the counts above cannot express. 11.3.1 reads
# "MSFT plus a mid-cap plus an ETF": two of those three slots name a kind and
# one names a ticker, so only the ticker is checkable here. QQQ is the ETF the
# rest of this repository already carries a day of (validation/QQQ_*.json), so
# it is the specific name whose absence is checkable rather than a category the
# script would have to judge. Transcribed once, like the prediction bars.
PREREG_SLOTS = ["MSFT", "a mid-cap", "an ETF"]
PREREG_SYMBOLS = ["MSFT", "QQQ"]

# The bars, transcribed once from docs/build-plan-9-12.md 11.3. Nothing below
# writes a verdict; every verdict is computed against these.
P1_INVENTORY_CUT = 0.30     # max|q| lower by at least 30%
P2_MARKOUT_EQUIV = 0.10     # within 10% of baseline counts as indistinguishable
P3_NEGATIVE_LANES = 3       # equity/share negative in >= 3 of 4 lanes
MAJORITY = 0.5              # "on most symbol-days"

# The minimum price increment, transcribed once like the bars above rather than
# derived: Reg NMS Rule 612 sets it at $0.01 for NMS stocks quoted at or above
# $1, and every symbol evaluated here traded far above that on every day in the
# sample. It is a fact about the venue, not a measurement, and it is the only
# constant in the conclusion that does not come out of an artifact -- which is
# why it is here, named, instead of inline as 0.01.
TICK = 0.01


def plural(n, word, suffix="s"):
    """`1 symbol`, `3 symbols`. Trivial, and it is here because the first draft
    of the conclusion read "Over 1 symbols" on a one-symbol run -- which is the
    smallest possible version of the failure this whole file exists to prevent:
    generated prose asserting something the inputs did not say."""
    return f"{n:,} {word}{'' if n == 1 else suffix}"


def most(hits, total):
    """+1 if the claim holds on a majority, -1 if it holds on none, else 0.

    EVERY DIRECTIONAL SENTENCE IN THE CONCLUSION BRANCHES ON ONE OF THESE.
    The first draft computed the counts correctly and then stated the direction
    in a fixed string -- so on a synthetic feed where A-S beat the baseline the
    page read "0 of 4 cells" immediately above "it is a different market, made
    worse". A generated conclusion that can only conclude one thing is a
    hand-written conclusion with extra steps."""
    if total == 0:
        return 0
    if hits > total / 2:
        return 1
    if hits == 0:
        return -1
    return 0


def cell_verdict(hits, total, bar=MAJORITY):
    """kept / mixed / falsified from a fraction of cells meeting a bar."""
    if total == 0:
        return "not evaluated", 0.0
    frac = hits / total
    if frac >= bar:
        return "kept", frac
    if hits == 0:
        return "falsified", frac
    return "mixed", frac


def rel_band(vals):
    """(max - min) / median|v| -- the band width, scaled so lanes compare."""
    vals = [v for v in vals if v is not None]
    if len(vals) < 2:
        return None
    scale = statistics.median([abs(v) for v in vals])
    if scale == 0:
        return None
    return (max(vals) - min(vals)) / scale


def subject_of(figure):
    """What symbol-day a figure was drawn from, per the manifest that
    scripts/paper-figures.sh writes beside it. The plotter picks the symbol-day
    and the caption reports it, so the two must not be two separate rules --
    they are one rule, recorded once, read here."""
    m = load(MANIFEST)
    if not m:
        return ""
    for f in m.get("figures", []):
        if f["figure"] == figure:
            return f.get("subject", "")
    return ""


# EVERY read and write in this file names its encoding and its newline, and the
# reason is a gate that could not pass on the machine it was run on. Python's
# default text encoding is the locale's, which is cp1252 on Windows: read_text()
# decoded this paper's lambda and its en-dashes into mojibake, the generated
# blocks came back as correct UTF-8, and --check reported the paper stale over a
# difference that existed only inside the process. write_text() would then have
# translated every LF to CRLF against a .gitattributes that exists to stop
# exactly that. Same family as the size_t/PRIu64 bug in build-plan 11.4: green
# CI on one platform is not portability.
def load(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------- calibration

def load_calibrations():
    """Every committed calibration, keyed by the symbol the artifact names.

    One file per symbol, because k is fitted per symbol: measured spreads on a
    single real session ran from 1.0 ticks to 60.7, and a curve fitted on one
    of those describes nothing about the other."""
    out = {}
    for path in sorted(glob.glob(CALIB_GLOB)):
        d = load(path)
        if d is None or "lanes" not in d:
            continue
        out[d.get("symbol", Path(path).stem)] = d
    return out


def build_calibration(cals):
    L = ["<!-- generated:calibration:begin -->", ""]
    if not cals:
        L += ["> **Not measured.** No `validation/intensity*.json` is committed, so no "
              "fitted A or k appears here. One artifact per symbol, because k is fitted "
              "per symbol:",
              ">",
              "> ```",
              "> build/calibrate_intensity data/sliced/SYM-DAY.gz \\",
              ">     --symbol SYM --day YYYY-MM-DD \\",
              ">     --json validation/intensity-SYM.json",
              "> ```",
              ">",
              "> Until they exist, §5's spread formula is being fed the **default** k "
              "rather than a measured one, §6.1's per-lane decision has nothing to be "
              "per-lane about, and the experiment driver refuses to run without "
              "`--allow-assumed-k`.", "",
              "<!-- generated:calibration:end -->"]
        return "\n".join(L)

    L += ["k is fitted **per symbol and per lane**, and both dimensions are "
          "load-bearing. Per lane because λ̂ is estimated *through* a queue model and "
          "is therefore conditional on it (§6.1). Per symbol because measured spreads "
          "on a single session ran from 1.0 ticks to 60.7 — a curve fitted on one of "
          "those describes nothing about the other.", ""]

    for sym, d in cals.items():
        lanes = d.get("lanes", {})
        L += [f"**{sym}** · calibrated {d.get('day', 'day not recorded')} · "
              f"`calibrated_per_lane`: "
              f"`{str(d.get('calibrated_per_lane', False)).lower()}`", "",
              "| lane | A | k (1/$) | R² | buckets fitted | exposure, no fills | "
              "maker fills | exposure (order-seconds) |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for m in MODELS:
            c = lanes.get(m)
            if c is None:
                continue
            if not c.get("fit_ok"):
                L.append(f"| **{m}** | — | — | — | {c['buckets_fitted']} | "
                         f"{c['buckets_no_fills']} | {c['maker_fills']:,} | "
                         f"{c['exposure_order_seconds']:,.0f} |")
                continue
            L.append(f"| **{m}** | {c['A']:.4f} | {c['k']:.1f} | {c['r_squared']:.3f} | "
                     f"{c['buckets_fitted']} | {c['buckets_no_fills']} | "
                     f"{c['maker_fills']:,} | {c['exposure_order_seconds']:,.0f} |")
        fitted = [lanes[m] for m in MODELS if m in lanes and lanes[m].get("fit_ok")]
        if fitted:
            ks = [c["k"] for c in fitted]
            lo, hi = min(ks), max(ks)
            # TWO FITTED LANES OR NO SPREAD SENTENCE. With one, this printed
            # "k spans 483.9 to 483.9 -- 1.00x" and then delivered the
            # cross-lane cost paragraph as though four lanes had disagreed by
            # nothing, when in fact three of them refused to fit at all. A
            # referee caught it (docs/paper/review/, minor 1). The degenerate
            # case is not a spread of zero; it is the absence of a spread.
            if len(fitted) >= 2:
                L += ["", f"Across lanes k spans {lo:.1f} to {hi:.1f}"
                          f"{f' — {hi / lo:.2f}×' if lo else ''}. That factor is the "
                          f"§6.1 cost as a number: one fit reused across four lanes "
                          f"hands three of them a curve from a market they do not "
                          f"live in."]
            else:
                only = next(m for m in MODELS
                            if m in lanes and lanes[m].get("fit_ok"))
                L += ["", f"Only the **{only}** lane fitted, so there is no cross-lane "
                          f"spread to report — k = {lo:.1f} is one number from one "
                          f"world, not a range. The other three lanes are refusals and "
                          f"are counted as such in §8.4; this symbol is not evaluated "
                          f"in §7."]
            drops = sum(c["buckets_no_fills"] for c in fitted)
            if drops:
                L += ["", f"{drops} bucket(s) had exposure and no fills and are "
                          f"excluded from the fit. Reported because dropping them "
                          f"silently flattens the curve — the deep buckets are exactly "
                          f"the ones that fail to fill."]
        L.append("")

    allk = [c["k"] for d in cals.values() for c in d.get("lanes", {}).values()
            if c.get("fit_ok")]
    if len(cals) > 1 and allk:
        L += [f"Across all {len(cals)} symbols and their lanes, k spans {min(allk):.1f} "
              f"to {max(allk):.1f}. A single scalar over that range is not a parameter.",
              ""]

    # WHAT THE MEASUREMENT WAS MEASURED THROUGH. The exposure denominator is
    # generated by running the A-S maker itself, which needs a k before there is
    # one -- so every number above is one step of a fixed-point map from a
    # bootstrap, and the paper said so nowhere until a referee said it for us
    # (docs/paper/review/, M1). The parameters were always in the artifacts;
    # being recorded and being disclosed are different properties, and only the
    # first one was true.
    boot = {(c.get("gamma"), c.get("k_assumed")) for c in cals.values()
            if c.get("k_assumed") is not None}
    bg, bk, far = None, None, []
    if len(boot) == 1:
        bg, bk = next(iter(boot))
        for sym, c in cals.items():
            ks = [ln["k"] for ln in c.get("lanes", {}).values() if ln.get("fit_ok")]
            if ks and bk:
                km = statistics.median(ks)
                far.append((max(km / bk, bk / km), sym, km))
        far.sort(reverse=True)
    if far:
        moved = "; ".join(f"{s} {r:.1f}×" for r, s, _ in far)
        # The sting is COMPUTED, not asserted. The floor (2/γ)·ln(1 + γ/k) is
        # monotone decreasing in k, so the widest-floor symbol is the
        # smallest-k one -- and whether that is also the symbol furthest from
        # its own bootstrap is a fact about this data, not a rhetorical flourish
        # that would survive the data changing under it.
        widest = min(far, key=lambda x: x[2])[1]
        sting = (f" — so the symbol furthest from its own starting point is "
                 f"**{far[0][1]}**, which is also the smallest measured k here and "
                 f"therefore the symbol carrying §8.3's wide-floor case"
                 if widest == far[0][1] else
                 f" — the symbol furthest from its own starting point is "
                 f"**{far[0][1]}**, and the smallest measured k, which is the one "
                 f"§8.3's wide-floor case rests on, is **{widest}**")
        L += [f"**Every k above is one iteration, not a fixed point.** λ̂'s denominator "
              f"is exposure, and the exposure is generated by running the A-S maker "
              f"itself — which needs a k before one has been measured. All "
              f"{len(cals)} calibrations here bootstrap from γ = {bg:g} and "
              f"k = {bk:g}, recorded in each artifact as `gamma` and `k_assumed`. Where "
              f"an order rests, and therefore which depth buckets accumulate exposure, "
              f"is conditional on that guess; the map has not been iterated to "
              f"convergence and no convergence check exists. The distance travelled in "
              f"one step is not uniform — median fitted k against the bootstrap: "
              f"{moved}{sting}. A second pass under the measured parameters is the "
              f"check that settles it and it has not been run.", ""]
        # And the second half of the same conditioning, which no rerun fixes.
        L += ["Depth is integrated as the mid moves (§6), which is correct for exposure "
              "and is **not** the conditional distribution A-S consumes. λ(δ) enters the "
              "spread formula as the fill rate for an order *placed* at δ; much of the "
              "exposure at large δ here comes instead from orders the mid walked away "
              "from, which are adversely selected moments. The two are different "
              "conditionings and the bias between them is not shown to be neutral. "
              "Splitting the estimate into placed-at-depth and drifted-to-depth would "
              "test it; that split does not exist today.", ""]

    lanes = next(iter(cals.values())).get("lanes", {})

    fig = FIGDIR / "intensity-mbo.svg"
    if fig.exists():
        subj = subject_of("intensity-mbo.svg")
        L += ["", f"![Observed vs fitted fill intensity, mbo lane]({FIGREL}/intensity-mbo.svg)",
              "",
              f"*Observed ln λ̂ per depth bucket against the fitted A·e^(−kδ)"
              f"{', ' + subj if subj else ''}. The touch bucket is the §6.2 misfit.*"]
        touch = None
        res = lanes.get("mbo", {}).get("residuals", [])
        for r in res:
            if r["ticks"] == 0:
                touch = r
        if touch is not None:
            sign = "below" if touch["residual"] < 0 else "above"
            L += ["", f"At the touch the observed intensity sits {abs(touch['residual']):.2f} "
                      f"in ln λ {sign} the fitted curve. §6.2 predicted below, and the "
                      f"direction is the claim — an exponential in depth alone has no way to "
                      f"express queue position."]
    L += ["", "<!-- generated:calibration:end -->"]
    return "\n".join(L)


# -------------------------------------------------------------------- results

def pick(runs, sym, day, arm, model, gamma=None):
    for r in runs:
        if (r["symbol"] == sym and r["day"] == day and r["arm"] == arm
                and r["model"] == model
                and (gamma is None or abs(r["gamma"] - gamma) < 1e-12)):
            return r
    return None


def no_results_block():
    return "\n".join([
        "<!-- generated:results:begin -->",
        "",
        "> **No results.** `validation/as-experiment.json` is not committed, so this",
        "> section is empty *by construction*, not by omission. `scripts/paper-report.py`",
        "> emits a results table only from a committed artifact, and there is no path by",
        "> which a number reaches this page without one.",
        ">",
        "> The artifact needs, before it can back this section:",
        ">",
        f"> - **≥ {MIN_SYMBOLS} symbols** across the liquidity spectrum and "
        f"**≥ {MIN_EVAL_DAYS} evaluation days**",
        "> - the **calibration day excluded** from evaluation — the harness exits",
        ">   non-zero rather than warning if they overlap",
        "> - four closed-loop lanes per arm per symbol-day (§4: a band over *worlds*)",
        "> - **a calibration artifact per symbol**, fitted on the calibration day. k is",
        ">   per symbol and per lane; the driver refuses to run without one unless",
        ">   `--allow-assumed-k` is passed, which stamps the output `assumed-scalar`.",
        ">",
        "> ```",
        "> bench/as-experiment.py --build build --out validation/as-experiment.json \\",
        ">     --feed SYM:YYYY-MM-DD:data/sliced/SYM-DAY.gz [--feed ...] \\",
        ">     --calibration SYM:validation/intensity-SYM.json [--calibration ...] \\",
        ">     --calibration-day YYYY-MM-DD",
        "> ```",
        ">",
        "> **The seven predictions in `docs/build-plan-9-12.md` §11.3 are ungraded.**",
        "> They were committed before the harness was written, and they will be graded",
        "> here — kept or falsified — by computation against the bars in this script,",
        "> not by anyone's reading of the table.",
        "",
        "<!-- generated:results:end -->",
    ])


def build_results(d, extra):
    if d is None:
        return no_results_block()

    runs = d["runs"]
    gammas = d["gammas"]
    positive = [g for g in gammas if g > 0]
    gsel = positive[len(positive) // 2] if positive else 0.0
    eval_days = d["evaluation_days"]
    symbols = d["symbols"]

    L = ["<!-- generated:results:begin -->", ""]

    # --- scope, checked against the pre-registered bar --------------------
    ok_scope = len(symbols) >= MIN_SYMBOLS and len(eval_days) >= MIN_EVAL_DAYS
    k_source = d.get("k_source", "assumed-scalar")
    k_by_symbol = d.get("k_by_symbol", {})
    L += [f"{len(symbols)} symbol(s) — {', '.join(symbols)} — over "
          f"{len(eval_days)} evaluation day(s), with {d['calibration_day']} held out as "
          f"the calibration day. Quote size {d['quote_size']}, modelled latency "
          f"{d['latency_ns']:,} ns. γ is swept over "
          f"{', '.join(f'{g:g}' for g in gammas)}; the tables below fix γ = {gsel:g} "
          f"and the sweep itself is the figure. That γ is **chosen by rule and not by "
          f"outcome** — `three_arm_cells` takes the middle of the swept positive grid, "
          f"whatever the grid is and whatever the numbers do — and §8.1 recounts the "
          f"headline at every swept γ so the choice can be checked rather than "
          f"trusted.", ""]

    if k_source != "measured-per-lane":
        L += [f"> **k was not measured for every symbol** (`k_source`: "
              f"`{k_source}`). Where no calibration artifact was supplied the runs used "
              f"the placeholder k = {d['k_measured']:g} in all four lanes, which means "
              f"§6.1's per-lane conditioning is **not in force** for them and the "
              f"corresponding rows below describe a strategy fed a fill curve from a "
              f"market it does not live in. This is a smoke-run configuration, not a "
              f"result.", ""]
    if k_by_symbol:
        L += ["k used, per symbol and lane — from the committed calibration artifacts, "
              "never from a flag typed by hand:", "",
              "| symbol | " + " | ".join(MODELS) + " | spread |",
              "|---|" + "---:|" * (len(MODELS) + 1)]
        for sym in symbols:
            ks = k_by_symbol.get(sym)
            if not ks:
                L.append(f"| {sym} | " + " | ".join("assumed" for _ in MODELS) +
                         f" | — |")
                continue
            vals = [ks[m] for m in MODELS if m in ks]
            span = f"{max(vals) / min(vals):.2f}×" if vals and min(vals) else "—"
            L.append(f"| {sym} | " +
                     " | ".join(f"{ks[m]:.1f}" if m in ks else "—" for m in MODELS) +
                     f" | {span} |")
        L.append("")
    if not ok_scope:
        L += [f"> **Below the pre-registered scope.** The plan's done-list requires "
              f"≥ {MIN_SYMBOLS} symbols × ≥ {MIN_EVAL_DAYS} evaluation days; this "
              f"artifact has {len(symbols)} × {len(eval_days)}. The tables are printed "
              f"because they are what was measured, and the predictions below are graded "
              f"against them, but **the phase's done-condition is not met** and no "
              f"conclusion here should be read as though it were.", ""]

    # --- 7.1 headline, per symbol-day ------------------------------------
    #
    # THE DRIFT SENTENCE IS COMPUTED, and it was not always. It carried a
    # hand-typed "94% of equity" inside this generator -- a transcribed number
    # in the one file whose entire job is to stop numbers being transcribed --
    # and by the time anyone looked, the artifact said 86%. The lesson is that
    # standing rule 7 applies to the generator as hard as to the document: a
    # literal in here is indistinguishable from a measurement in the output.
    #
    # The symbol named is chosen ON THE CLAIM BEING MADE -- the one where the
    # drift range is widest RELATIVE TO the edge range, which is exactly "drift
    # is the noisy term and edge is the steady one".
    #
    # Selecting on |drift| / |equity| instead, as the first version did, picks
    # whichever symbol's equity landed nearest zero: STOR scored 129% there,
    # because its drift and edge partly cancel, and the sentence came out
    # naming the least illustrative case in the sample. A ratio with a small
    # denominator is not evidence of a large numerator.
    #
    # Units stay micro-dollars per share, as everywhere else in this section.
    # Rendering these in dollars at two decimals reported STOR's edge as
    # "between $0.00 and $0.00", which is a true statement that destroys the
    # number it is about.
    drift_note = ""
    ranked = []
    for sym in symbols:
        cs = [r for r in runs if r["symbol"] == sym and r["arm"] == "symmetric-touch"
              and r["day"] in eval_days and r["gamma"] == 0.0
              and r["equity_per_share_micros"]]
        if len(cs) < 2:
            continue
        dr = [r["drift_per_share_micros"] for r in cs]
        eg = [r["edge_per_share_micros"] for r in cs]
        erange = max(eg) - min(eg)
        if erange <= 0:
            continue
        ranked.append(((max(dr) - min(dr)) / erange, sym, cs, dr, eg))
    if ranked:
        ratio, dsym, cs, dr, eg = max(ranked)
        share = statistics.median([abs(r["drift_per_share_micros"])
                                   / abs(r["equity_per_share_micros"]) for r in cs])
        drift_note = (
            f" On {dsym} drift is a median {share:.0%} of equity and ranges over "
            f"{min(dr):,} to {max(dr):,} µ$ per share across days — a spread "
            f"{ratio:,.0f}× as wide as the edge's own {min(eg):,} to {max(eg):,}. "
            f"One is a property of the strategy, the other of the stock.")
    L += ["### 7.1 Headline band, per symbol-day", "",
          "**Edge, not equity, is the market-making result.** `equity = edge + drift − "
          "fees`: edge is half-spread captured against the mid at fill time, drift is "
          "what the mid did to inventory, including the residual position marked at "
          "the close." + drift_note + " "
          "Equity is still shown, because the pre-registered predictions were written "
          "against it and are graded against it in §7.5.", "",
          "All figures µ$ per share. `mk 1s` is the 1-second markout — negative is "
          "adverse selection. Never pooled: each table is one symbol-day.", ""]
    for sym in symbols:
        for day in eval_days:
            rows = []
            for m in MODELS:
                t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                s = pick(runs, sym, day, "as", m, gsel)
                if not (t and z and s):
                    continue
                rows.append((m, t, z, s))
            if not rows:
                continue
            L += [f"**{sym} · {day}**", "",
                  "| lane | **edge touch** | **edge γ=0** | **edge A-S** | eq touch | "
                  "eq A-S | max\\|q\\| touch | max\\|q\\| A-S | mk 1s A-S |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
            for m, t, z, s in rows:
                L.append(f"| {m} | {t.get('edge_per_share_micros', 0):,} | "
                         f"{z.get('edge_per_share_micros', 0):,} | "
                         f"{s.get('edge_per_share_micros', 0):,} | "
                         f"{t['equity_per_share_micros']:,} | "
                         f"{s['equity_per_share_micros']:,} | "
                         f"{t['inv_max_abs']:,} | {s['inv_max_abs']:,} | "
                         f"{s['markout_1s']:,} |")
            band_t = rel_band([t.get("edge_per_share_micros", 0) for _, t, _, _ in rows])
            band_s = rel_band([s.get("edge_per_share_micros", 0) for _, _, _, s in rows])
            if band_t is not None and band_s is not None:
                L += ["", f"Edge band width (max − min over the four lanes, scaled by the "
                          f"median |edge|): touch-maker {band_t:.2f}, A-S {band_s:.2f}."]
            L.append("")

    # --- 7.2 mechanism ----------------------------------------------------
    L += ["### 7.2 Mechanism: which gap is which", "",
          "`touch → γ=0` is the **spread choice**; `γ=0 → A-S` is the **inventory "
          "skew**. A two-arm comparison bundles them, and the bundled number is what "
          "gets reported as \"A-S wins\". Δ is A-S-side minus baseline-side.", "",
          "**Decomposed on edge.** Differencing equity between arms differences their "
          "drift too, and the arms hold deliberately different amounts of inventory — "
          "the skew arm exists to hold less. Scoring the mechanism on a drift-carrying "
          "metric would credit or blame the treatment for the stock's direction.", "",
          "| symbol | day | lane | Δedge spread | Δedge skew | Δinv sd spread | Δinv sd skew |",
          "|---|---|---|---:|---:|---:|---:|"]
    for sym in symbols:
        for day in eval_days:
            for m in MODELS:
                t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                s = pick(runs, sym, day, "as", m, gsel)
                if not (t and z and s):
                    continue
                et = t.get("edge_per_share_micros", 0)
                ez = z.get("edge_per_share_micros", 0)
                es = s.get("edge_per_share_micros", 0)
                L.append(f"| {sym} | {day} | {m} | {ez - et:,} | {es - ez:,} | "
                         f"{z['inv_stdev'] - t['inv_stdev']:,.1f} | "
                         f"{s['inv_stdev'] - z['inv_stdev']:,.1f} |")
    L.append("")

    # --- 7.3 day-level spread --------------------------------------------
    L += ["### 7.3 Day-level spread", "",
          "The spread across days **is** the result. No mean is taken: with this many "
          "symbol-days a mean invites a claim the data cannot support. Edge and equity "
          "side by side, because the difference between their ranges is the point — "
          "the day-to-day range of equity is mostly the day-to-day range of the "
          "stock.", "",
          "| symbol | lane | arm | days | min edge | max edge | min eq | max eq |",
          "|---|---|---|---:|---:|---:|---:|---:|"]
    for sym in symbols:
        for m in MODELS:
            for arm, g in (("symmetric-touch", 0.0), ("as", gsel)):
                sel = [r for r in runs
                       if r["symbol"] == sym and r["model"] == m and r["arm"] == arm
                       and not r["is_calibration_day"] and abs(r["gamma"] - g) < 1e-12]
                if not sel:
                    continue
                ed = [r.get("edge_per_share_micros", 0) for r in sel]
                eq = [r["equity_per_share_micros"] for r in sel]
                L.append(f"| {sym} | {m} | {arm} | {len(sel)} | {min(ed):,} | "
                         f"{max(ed):,} | {min(eq):,} | {max(eq):,} |")
    L.append("")

    # --- 7.4 the sweep figures -------------------------------------------
    figs = [("gamma-inventory.svg", "γ against inventory, log-log, one line per lane"),
            ("gamma-pnl.svg", "γ against equity per share, linear with zero in range")]
    present = [(f, cap) for f, cap in figs if (FIGDIR / f).exists()]
    if present:
        L += ["### 7.4 The γ sweep", "",
              "γ is swept and plotted rather than chosen. Inventory is log-log because it "
              "spans decades; P&L is linear **with zero in range**, because P&L is read by "
              "its sign and an axis that crops zero makes a small positive number look "
              "large.", ""]
        for f, cap in present:
            subj = subject_of(f)
            L += [f"![{cap}]({FIGREL}/{f})", "",
                  f"*{cap}{' — ' + subj if subj else ''}. One symbol-day: the sweep is "
                  f"not pooled either.*", ""]

    # --- 7.5 the predictions, graded --------------------------------------
    L += ["### 7.5 The predictions, graded", "",
          "Committed in `docs/build-plan-9-12.md` §11.3 before the harness existed. Each "
          "verdict below is **computed** from the bar stated beside it, not written by "
          "hand — a report script that prints \"kept\" from a string literal has already "
          "happened once here.", "",
          "| # | claim | bar | cells meeting it | verdict |",
          "|---|---|---|---:|---|"]

    def cells():
        for sym in symbols:
            for day in eval_days:
                for m in MODELS:
                    t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                    z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                    s = pick(runs, sym, day, "as", m, gsel)
                    if t and z and s:
                        yield sym, day, m, t, z, s

    allc = list(cells())

    # P1 -- inventory excursions at least 30% smaller.
    hits = sum(1 for _, _, _, t, _, s in allc
               if t["inv_max_abs"] > 0
               and (t["inv_max_abs"] - s["inv_max_abs"]) / t["inv_max_abs"] >= P1_INVENTORY_CUT)
    v1, f1 = cell_verdict(hits, len(allc))
    L.append(f"| P1 | A-S carries smaller inventory excursions | max\\|q\\| lower by "
             f"≥ {P1_INVENTORY_CUT:.0%} | {hits}/{len(allc)} ({f1:.0%}) | **{v1}** |")

    # P2 -- markouts indistinguishable.
    same = 0
    counted = 0
    for _, _, _, t, _, s in allc:
        base = abs(t["markout_1s"])
        if base == 0:
            continue
        counted += 1
        if abs(s["markout_1s"] - t["markout_1s"]) / base <= P2_MARKOUT_EQUIV:
            same += 1
    v2, f2 = cell_verdict(same, counted)
    L.append(f"| P2 | the gain is inventory variance, **not** markout | 1s markout within "
             f"{P2_MARKOUT_EQUIV:.0%} of baseline | {same}/{counted} ({f2:.0%}) | **{v2}** |")

    # P3 -- A-S loses money in >= 3 of 4 lanes on most symbol-days.
    days_hit = 0
    days_total = 0
    for sym in symbols:
        for day in eval_days:
            lanes_neg = sum(1 for m in MODELS
                            if (r := pick(runs, sym, day, "as", m, gsel))
                            and r["equity_per_share_micros"] < 0)
            present_lanes = sum(1 for m in MODELS if pick(runs, sym, day, "as", m, gsel))
            if present_lanes == 0:
                continue
            days_total += 1
            if lanes_neg >= P3_NEGATIVE_LANES:
                days_hit += 1
    v3, f3 = cell_verdict(days_hit, days_total)
    L.append(f"| P3 | A-S loses money after fees | equity/share < 0 in ≥ "
             f"{P3_NEGATIVE_LANES} of 4 lanes | {days_hit}/{days_total} symbol-days "
             f"({f3:.0%}) | **{v3}** |")

    # P4 -- latency degradation, graded only across artifacts at different latencies.
    v4 = v5 = "not evaluated"
    p4_companion = []       # emitted after the table; see below
    lat = sorted(extra.items())
    if len(lat) < 2:
        L.append(f"| P4 | A-S degrades faster with latency | fractional equity loss "
                 f"larger than baseline's | one latency only ({d['latency_ns']:,} ns) | "
                 f"**not evaluated** |")
    else:
        (lo_ns, lo_d), (hi_ns, hi_d) = lat[0], lat[-1]
        worse = 0
        tot = 0
        for sym in symbols:
            for day in eval_days:
                for m in MODELS:
                    a0 = pick(lo_d["runs"], sym, day, "as", m, gsel)
                    a1 = pick(hi_d["runs"], sym, day, "as", m, gsel)
                    b0 = pick(lo_d["runs"], sym, day, "symmetric-touch", m, 0.0)
                    b1 = pick(hi_d["runs"], sym, day, "symmetric-touch", m, 0.0)
                    if not (a0 and a1 and b0 and b1):
                        continue
                    if a0["equity_per_share_micros"] == 0 or b0["equity_per_share_micros"] == 0:
                        continue
                    tot += 1
                    da = (a0["equity_per_share_micros"] - a1["equity_per_share_micros"]) \
                        / abs(a0["equity_per_share_micros"])
                    db = (b0["equity_per_share_micros"] - b1["equity_per_share_micros"]) \
                        / abs(b0["equity_per_share_micros"])
                    if da > db:
                        worse += 1
        v4, f4 = cell_verdict(worse, tot)
        L.append(f"| P4 | A-S degrades faster with latency | fractional loss from "
                 f"{lo_ns:,} to {hi_ns:,} ns larger than baseline's | {worse}/{tot} "
                 f"({f4:.0%}) | **{v4}** |")

        # THE SAME TEST ON EDGE, REPORTED BESIDE THE VERDICT AND NEVER INSTEAD
        # OF IT.
        #
        # P4 was pre-registered against equity per share, so equity is what
        # grades it -- a bar that moves once the data is in is not a bar. But
        # `equity = edge + drift - fees` and section 7.1 measures how much of
        # that is drift; a fractional EQUITY change between two latencies is
        # therefore substantially a change in what the stock did to two
        # different inventory paths, which is a fact about the stock.
        #
        # So the companion below runs the identical comparison on EDGE -- the
        # half-spread the maker actually captured, which is the part latency can
        # plausibly act on -- and says in as many words that it is not the
        # pre-registered bar. Two honest numbers, and the reader can see why
        # they differ. Substituting it silently would be moving the bar; leaving
        # it out would be reporting the noisier of two instruments because it
        # happened to be named first.
        # PER SYMBOL, NEVER POOLED -- section 7 says so about every other table
        # here and the first version of this one ignored it. Pooling 3 symbols
        # into 36 cells reported A-S degrading faster overall and hid that STOR
        # reverses: there the baseline degrades faster. An average over symbols
        # is exactly the claim the sample cannot support.
        arms = [("symmetric-touch", 0.0), ("as-gamma0", 0.0), ("as", gsel)]
        rows = []
        for sym in symbols:
            for arm, g in arms:
                e_lo, e_hi, fracs = [], [], []
                for day in eval_days:
                    for m in MODELS:
                        r0 = pick(lo_d["runs"], sym, day, arm, m, g)
                        r1 = pick(hi_d["runs"], sym, day, arm, m, g)
                        if not (r0 and r1):
                            continue
                        e_lo.append(r0["edge_per_share_micros"])
                        e_hi.append(r1["edge_per_share_micros"])
                        if r0["edge_per_share_micros"]:
                            fracs.append((r0["edge_per_share_micros"]
                                          - r1["edge_per_share_micros"])
                                         / abs(r0["edge_per_share_micros"]))
                if e_lo:
                    rows.append((sym, arm, statistics.median(e_lo),
                                 statistics.median(e_hi),
                                 statistics.median(fracs) if fracs else None))

        # The headline count, and the same count per symbol -- because "A-S
        # degrades faster" is a claim that can hold overall and fail on a
        # symbol, and here it does.
        def faster(sym_filter):
            w = t = 0
            for sym in symbols:
                if sym_filter and sym != sym_filter:
                    continue
                for day in eval_days:
                    for m in MODELS:
                        a0 = pick(lo_d["runs"], sym, day, "as", m, gsel)
                        a1 = pick(hi_d["runs"], sym, day, "as", m, gsel)
                        b0 = pick(lo_d["runs"], sym, day, "symmetric-touch", m, 0.0)
                        b1 = pick(hi_d["runs"], sym, day, "symmetric-touch", m, 0.0)
                        if not (a0 and a1 and b0 and b1):
                            continue
                        if not a0["edge_per_share_micros"] or not b0["edge_per_share_micros"]:
                            continue
                        t += 1
                        da = (a0["edge_per_share_micros"] - a1["edge_per_share_micros"]) \
                            / abs(a0["edge_per_share_micros"])
                        db = (b0["edge_per_share_micros"] - b1["edge_per_share_micros"]) \
                            / abs(b0["edge_per_share_micros"])
                        if da > db:
                            w += 1
            return w, t

        worse_e, tot_e = faster(None)
        per_sym = {sym: faster(sym) for sym in symbols}
        holds = [sym for sym, (w, t) in per_sym.items() if t and w > t / 2]
        agree = "agrees with" if (worse_e > tot_e / 2) == (worse > tot / 2) else \
                "**disagrees with**"
        p4_companion = [
            f"> **P4's bar is equity, and §7.1 measures how much of equity is drift.** "
            f"P4 was pre-registered against equity per share and is graded against it "
            f"above; the bar does not move once the data is in. But a fractional "
            f"*equity* change between two latencies is substantially a change in what "
            f"the stock did to two different inventory paths. The identical test on "
            f"**edge** — the half-spread the maker actually captured, which is the part "
            f"latency can act on — is reported here. It is **not** the pre-registered "
            f"bar and does not replace the verdict above; it {agree} it.",
            ">",
            f"> | symbol | arm | median edge/share at {lo_ns:,} ns | at {hi_ns:,} ns | "
            f"median per-cell change |",
            "> |---|---|---:|---:|---:|"]
        for sym, arm, m_lo, m_hi, frac in rows:
            p4_companion.append(
                f"> | {sym} | `{arm}` | {m_lo:+,.0f} | {m_hi:+,.0f} | "
                + (f"{-frac:+.0%} |" if frac is not None else "— |"))
        p4_companion += [
            ">",
            "> *The last column is the median of the per-cell fractional changes, not "
            "the change between the two medians beside it. They do not compose, and "
            "where edge crosses zero between the two latencies they differ by a lot — "
            "a cell going +100 to −100 is a −200% change on a base the level columns "
            "barely move.*",
            ">",
            f"> A-S's edge degrades faster than the baseline's in **{worse_e} of "
            f"{tot_e}** cells overall, against {worse} of {tot} on equity — and per "
            f"symbol it holds on "
            + (f"{', '.join(holds)} but **not** on "
               f"{', '.join(sym for sym in symbols if sym not in holds)}"
               if holds and len(holds) < len(symbols) else
               f"{'all ' + str(len(symbols)) if holds else 'none'} of them")
            + ". "
            + ", ".join(f"{sym} {w}/{t}" for sym, (w, t) in per_sym.items())
            + ". `as-gamma0` is in the table because it separates the two halves of the "
              "model: if the latency damage is in the spread choice rather than the "
              "skew, that row shows it with no inventory effect in the way.",
            ""]

    # P5 -- the closed-loop band is wider than phase 6's open-loop band.
    p6 = load(PHASE6)
    if p6 is None:
        L.append("| P5 | the band over *worlds* is wider than the band over *gradings* | "
                 "closed-loop band > phase-6 band | phase-6 artifact missing | "
                 "**not evaluated** |")
    else:
        open_band = rel_band([p6["models"][m]["per_share_micros"]
                              for m in MODELS if m in p6.get("models", {})])
        wider = 0
        tot = 0
        for sym in symbols:
            for day in eval_days:
                vals = [r["equity_per_share_micros"] for m in MODELS
                        if (r := pick(runs, sym, day, "as", m, gsel))]
                cb = rel_band(vals)
                if cb is None or open_band is None:
                    continue
                tot += 1
                if cb > open_band:
                    wider += 1
        v5, f5 = cell_verdict(wider, tot)
        L.append(f"| P5 | the band over *worlds* is wider than the band over *gradings* | "
                 f"closed-loop band > phase-6 band ({open_band:.2f}) | {wider}/{tot} "
                 f"({f5:.0%}) | **{v5}** |")

    # P6 -- shape: inventory monotone in gamma, P&L flatter than the lane band.
    mono = 0
    flat = 0
    shape_tot = 0
    for sym in symbols:
        for day in eval_days:
            for m in MODELS:
                series = [pick(runs, sym, day, "as", m, g) for g in positive]
                if any(r is None for r in series) or len(series) < 2:
                    continue
                shape_tot += 1
                inv = [r["inv_max_abs"] for r in series]
                if all(b <= a for a, b in zip(inv, inv[1:])):
                    mono += 1
                eq = [r["equity_per_share_micros"] for r in series]
                sweep_band = rel_band(eq)
                lane_vals = [r["equity_per_share_micros"] for mm in MODELS
                             if (r := pick(runs, sym, day, "as", mm, positive[len(positive) // 2]))]
                lane_band = rel_band(lane_vals)
                if sweep_band is not None and lane_band is not None and sweep_band < lane_band:
                    flat += 1
    v6a, f6a = cell_verdict(mono, shape_tot)
    v6b, f6b = cell_verdict(flat, shape_tot)
    v6 = "kept" if v6a == "kept" and v6b == "kept" else (
        "falsified" if v6a == "falsified" and v6b == "falsified" else "mixed")
    if shape_tot == 0:
        v6 = "not evaluated"
    L.append(f"| P6 | γ moves inventory a lot and P&L little | max\\|q\\| monotone ↓ in γ "
             f"**and** P&L sweep band < lane band | {mono}/{shape_tot} monotone, "
             f"{flat}/{shape_tot} flat | **{v6}** |")

    # P7 -- the falsification condition for the phase.
    indistinguishable = (v1 == "falsified" and v6 == "falsified" and v2 == "kept")
    v7 = "TRIGGERED" if indistinguishable else "not triggered"
    L.append(f"| P7 | if A-S and baseline are the same on every axis, *that* is the "
             f"finding | P1 and P6 falsified while P2 holds | "
             f"P1 {v1}, P2 {v2}, P6 {v6} | **{v7}** |")
    L.append("")

    if indistinguishable:
        L += ["> **P7 is triggered.** Inventory-aware quoting did not separate from the "
              "naive baseline on inventory or on the γ sweep, while markouts stayed "
              "indistinguishable as predicted. The plan pre-committed to reporting this "
              "as a result rather than reframing it, and this line is emitted by the "
              "grader, not by an author deciding how to feel about the table.", ""]

    if p4_companion:
        L += p4_companion

    if p6 is not None:
        L += ["> The phase-6 artifact (`docs/figures/touch-maker.json`) does not record "
              "which symbol-day it was produced on, so P5 compares band *widths* and not "
              "the same feed twice. The comparison is scaled (max − min over the median "
              "|equity|) precisely so that it survives a change of symbol, but a reader "
              "should treat P5 as the weakest row in this table until phase 6 is re-run "
              "on a feed this paper also evaluates.", ""]

    verdicts = {"P1": v1, "P2": v2, "P3": v3, "P4": v4, "P5": v5, "P6": v6, "P7": v7}
    kept = sum(1 for v in verdicts.values() if v == "kept")
    fals = sum(1 for v in verdicts.values() if v == "falsified")
    mixed = sum(1 for v in verdicts.values() if v == "mixed")
    ungraded = sum(1 for v in verdicts.values() if v == "not evaluated")
    L += [f"Across all seven: {kept} kept, {fals} falsified, {mixed} mixed, "
          f"{ungraded} not evaluated "
          f"({', '.join(f'{k} {v}' for k, v in verdicts.items())}). Falsified "
          f"predictions stay on the page — the plan committed to grading them in print "
          f"whichever way they went, and phase 10.8's falsified P1 is still the most "
          f"useful thing in that section.", "",
          "<!-- generated:results:end -->"]
    return "\n".join(L)


# ------------------------------------------------------------------ the status
#
# The banner under the title is generated for a reason this repository has
# already paid for once: docs/phase10-results.md carried a hand-written caveat
# reading "UNMEASURABLE on this hardware" directly above a generated table
# reporting a measured bound. The prose was true when it was typed and false
# afterwards, and nothing could tell, because nothing regenerated it. A status
# line is the single most quotable sentence in a paper and the one most likely
# to be left behind by its own results, so it is computed too.


def build_status(d, cals):
    L = ["<!-- generated:status:begin -->", ""]
    if d is None:
        L += ["> **Status: methodology complete, results pending data.** Every section "
              "below that describes *how* is finished. Every section that reports *what "
              "happened* is generated from committed artifacts by "
              "`scripts/paper-report.py`, and those artifacts do not exist yet — the "
              f"evaluation needs at least {MIN_SYMBOLS} symbols across the liquidity "
              f"spectrum and at least {MIN_EVAL_DAYS} trading days, with the calibration "
              "day excluded. The generator refuses to emit a results table it does not "
              "have, so this document cannot accidentally look finished. See §7.", "",
              "<!-- generated:status:end -->"]
        return "\n".join(L)

    syms = d["symbols"]
    days = d["evaluation_days"]
    scope_ok = len(syms) >= MIN_SYMBOLS and len(days) >= MIN_EVAL_DAYS
    measured = d.get("k_source") == "measured-per-lane"
    L += [f"> **Status: run.** {len(syms)} symbols ({', '.join(syms)}) over "
          f"{len(days)} evaluation days from real NASDAQ TotalView-ITCH 5.0, with "
          f"{d['calibration_day']} held out to fit λ(δ)"
          f"{' from measured fills, per symbol and per lane' if measured else ''}. "
          f"{'That meets' if scope_ok else 'That is BELOW'} the pre-registered scope of "
          f"≥ {MIN_SYMBOLS} symbols and ≥ {MIN_EVAL_DAYS} evaluation days. Every number "
          f"and every verdict below §6 is generated from committed artifacts by "
          f"`scripts/paper-report.py`; none is typed. The conclusion is in §8 and the "
          f"pre-registered predictions are graded — kept and falsified alike — in §7.5.",
          ""]

    # COUNTS ARE NOT COMPOSITION, and this banner claimed scope compliance on
    # the half it could check. The pre-registration (build-plan 11.3.1) names a
    # kind for each slot -- MSFT, a mid-cap, an ETF -- and the ETF slot is empty:
    # no calibration artifact for one exists, so it was never even fitted. The
    # count test passed and the sentence read as though the whole bar had. A
    # referee found it (docs/paper/review/, M2); the banner now states the
    # unmet half rather than being silent about it.
    named = [s for s in PREREG_SYMBOLS if s not in syms]
    if named:
        L += [f"> **The scope is met on counts and not on composition.** The "
              f"pre-registration names a *kind* per slot — "
              f"{', '.join(PREREG_SLOTS)} — and evaluated here are "
              f"{', '.join(syms)}. Specifically {', '.join(named)} "
              f"{'is' if len(named) == 1 else 'are'} named in the plan and absent from "
              f"this paper, with no calibration artifact and therefore no refusal on "
              f"record either: not evaluated, and not declined. §8.5 carries it as open "
              f"work and `docs/paper/review/` carries the reason it went unstated for "
              f"as long as it did.", ""]
    L += ["<!-- generated:status:end -->"]
    return "\n".join(L)


# ------------------------------------------------- the abstract's two findings


def three_arm_cells(d):
    """Every (symbol, day, lane) in which all three arms ran, at the swept gamma.

    Shared by the abstract and the conclusion so the two cannot disagree about
    what the sample is -- an abstract that quotes a different denominator from
    the section it summarises is the oldest way for a paper to be wrong about
    itself."""
    runs = d["runs"]
    positive = [g for g in d["gammas"] if g > 0]
    gsel = positive[len(positive) // 2] if positive else 0.0
    out = []
    for sym in d["symbols"]:
        for day in d["evaluation_days"]:
            for m in MODELS:
                t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                a = pick(runs, sym, day, "as", m, gsel)
                if t and z and a:
                    out.append((sym, day, m, t, z, a))
    return out, gsel


def gamma_recount(d):
    """The headline count repeated at every swept gamma, not only the one the
    tables fix.

    A referee (docs/paper/review/, M-minor-5) pointed out that fixing gamma in
    the tables with no stated reason reads as a researcher degree of freedom
    even when it is not one, and that the recount converts it into a robustness
    statement for free. It is computed here rather than asserted because a
    robustness claim written by hand is the same failure as a verdict written by
    hand: it survives the numbers moving.

    Returns [(gamma, cells, edge_worse, edge_negative)], ordered by gamma."""
    out = []
    for g in [x for x in d["gammas"] if x > 0]:
        cells = []
        for sym in d["symbols"]:
            for day in d["evaluation_days"]:
                for m in MODELS:
                    t = pick(d["runs"], sym, day, "symmetric-touch", m, 0.0)
                    a = pick(d["runs"], sym, day, "as", m, g)
                    if t and a:
                        cells.append((t, a))
        if not cells:
            continue
        worse = sum(1 for t, a in cells
                    if a["edge_per_share_micros"] < t["edge_per_share_micros"])
        neg = sum(1 for _, a in cells if a["edge_per_share_micros"] < 0)
        out.append((g, len(cells), worse, neg))
    return out


def replication_units(d):
    """(symbol-days, symbols) -- the denominators a count of cells is NOT.

    Four lanes of one symbol-day are four scorings of a world that shares a
    feed, a day and a stock; on the widest-spread name here three of the four
    calibration lanes come back bit-identical (§6.3). Every count out of N cells
    in this document is followed by this, because a cell count reads as a sample
    size and is not one."""
    return len(d["symbols"]) * len(d["evaluation_days"]), len(d["symbols"])


def build_findings(d):
    """The abstract's result sentence. Generated, because the abstract is the
    part of a paper that gets quoted without the rest of it attached."""
    L = ["<!-- generated:findings:begin -->", ""]
    if d is None:
        L += ["The evaluation has not been run; §7 says exactly what it is waiting for, "
              "and no finding is stated anywhere in this document until it exists.", "",
              "<!-- generated:findings:end -->"]
        return "\n".join(L)
    cells, gsel = three_arm_cells(d)
    if not cells:
        L += ["The committed artifact has no cell in which all three arms ran, so no "
              "finding is stated here.", "", "<!-- generated:findings:end -->"]
        return "\n".join(L)
    n = len(cells)
    worse = sum(1 for *_, t, _, a in cells
                if a["edge_per_share_micros"] < t["edge_per_share_micros"])
    neg = sum(1 for *_, a in cells if a["edge_per_share_micros"] < 0)
    inv = statistics.median([(t["inv_max_abs"] - a["inv_max_abs"]) / t["inv_max_abs"]
                             for *_, t, _, a in cells if t["inv_max_abs"] > 0])
    mk_worse = sum(1 for *_, t, _, a in cells if a["markout_1s"] < t["markout_1s"])
    d_edge = most(worse, n)
    d_mk = most(mk_worse, n)

    answer = {1: "no", -1: "yes"}.get(d_edge, "not on this sample, either way")
    inv_clause = (f"while holding a median {inv:.0%} less inventory" if inv > 0
                  else f"while holding a median {abs(inv):.0%} **more** inventory")
    if d_edge == 1 and d_mk == 1 and inv > 0:
        trade = ("— it buys inventory control with adverse selection, which is the "
                 "reverse of the pre-registered prediction")
    elif d_edge == -1 and d_mk != 1:
        trade = ("— it captures more half-spread without being more adversely "
                 "selected, which is what the pre-registration expected")
    else:
        trade = "— the axes do not point the same way on this sample"
    L += [f"**The answer, over {plural(len(d['symbols']), 'symbol')} and "
          f"{plural(len(d['evaluation_days']), 'evaluation day')}, is "
          f"{answer}.** A-S captures less half-spread per share than a naive touch "
          f"maker in {worse} of {n} symbol-day-lane cells and goes outright negative "
          f"in {neg}, {inv_clause} {trade}. The mechanism runs through the tick: "
          f"measured k sets A-S's inventory-free half-spread, and §8.3 tests whether "
          f"the sign of its captured edge follows from whether that half-spread fits "
          f"on the venue's price grid. Assuming k, as implementations almost "
          f"universally do, is what keeps that question from being asked.", ""]
    sd, ns = replication_units(d)
    L += [f"{n} cells is **not** a sample of {n}. The four lanes of a symbol-day are "
          f"four scorings of one feed on one stock on one day; the replication unit "
          f"here is {sd} symbol-days, arguably {ns} symbols, and no count in this "
          f"document should be read as anything wider.", "",
          "<!-- generated:findings:end -->"]
    return "\n".join(L)


# -------------------------------------------------------------- the conclusion
#
# The conclusion is generated for the same reason the tables are: a hand-written
# summary is where a paper's numbers go to drift, and it is the section a reader
# is most likely to quote. Everything below is computed from the same artifacts
# the tables come from, including the direction of every claim -- there is no
# string in here that asserts a finding the arithmetic did not produce.
#
# And it refuses, like build_results does. A paper with no evaluation artifact
# gets a conclusion that says what it is waiting for, not a hedge that reads
# like a result.


def review_item():
    """§8.5's outside-reader item, generated from the committed review.

    The plan's one item that cannot be closed by writing code is §4 reviewed by
    someone who did not write it. An item that says "that has not happened" is
    correct until the day it is not, and then it is a false sentence nothing
    regenerates -- exactly the failure build_status exists to prevent, one
    section lower down. So the close is read off docs/paper/review/review.json:
    no committed review, no claim of one.

    And it reports the review's SHAPE, not just its existence. An unblinded
    reader who raised items adjacent to §4 without contesting its construction
    is real evidence and weak evidence, and a tick mark cannot tell the
    difference between that and a reader who engaged with the construction and
    found it sound."""
    r = load(REVIEW)
    if r is None:
        return ("2. **The outside reader.** The plan requires §4 to be reviewed by "
                "someone who did not write it. That has not happened, and it is the "
                "section most likely to be wrong in a way its author cannot see.")
    s4 = r.get("section_4", {})
    ind = r.get("independence", {})
    if not s4.get("reviewed"):
        return (f"2. **The outside reader — received, and §4 not reached.** A review is "
                f"committed under `docs/paper/review/` and did not cover §4, which is "
                f"the section the plan names. The item stays open.")
    items = ", ".join(s4.get("items", [])) or "none"
    conflict = ind.get("disclosed_conflict")
    blind = "not blind" if ind.get("blind") is False else "blind"
    nf = sum(1 for f in r.get("findings", []) if f.get("kind") != "question")
    nq = sum(1 for f in r.get("findings", []) if f.get("kind") == "question")
    contested = s4.get("contested_band_over_worlds")
    weight = ("and **contested the band-over-worlds construction itself**, which is "
              "argued in the response rather than conceded here"
              if contested else
              "and **did not contest the band-over-worlds construction itself**. That "
              "is evidence a competent reader with the code in front of them did not "
              "object to §4; it is not evidence that §4 is right, and the difference "
              "is the reason this item records the shape of the review and not a tick")
    return (f"2. **The outside reader — closed, with its limits stated.** §4 was "
            f"reviewed at `{r.get('reviewed_at_commit', 'an unrecorded commit')}` by a "
            f"reader who wrote neither the paper nor the code, verdict "
            f"*{r.get('verdict', 'unrecorded')}*. The reader was **{blind}** — they "
            + (f"{conflict}. " if conflict else "declared no conflict. ")
            + f"They raised {nf} findings and {nq} questions, of which {items} "
              f"bear on §4, {weight}. Report and point-by-point response: "
              f"`docs/paper/review/`.")


def open_review_item(n):
    """Every referee finding still open, listed in the paper's own what-remains.

    A response document that records four open findings while the paper's
    "what remains" lists none of them puts the honest version one directory away
    from the version a reader sees. The list is generated from the same
    review.json the item above reads, so a finding cannot be closed in the
    response and left open here, or the reverse."""
    r = load(REVIEW)
    if r is None:
        return None
    op = [f for f in r.get("findings", []) if str(f.get("state", "")).endswith("open")]
    if not op:
        return None
    # The OUTSTANDING WORK, not the original complaint. Several of these are
    # already fixed in the half a referee could see -- M2's banner now names the
    # empty ETF slot -- and listing the complaint verbatim would report a closed
    # item as open. `remaining` is what is left; a finding with none is a
    # bookkeeping error and is named as one rather than silently skipped.
    named = "; ".join(
        f"**{f['id']}** " + f.get("remaining", f"[no remaining work recorded: {f['title']}]")
        for f in op)
    total = len(r.get("findings", []))
    return (f"{n}. **The referee's open findings.** {len(op)} of {total} items from "
            f"the review at `{r.get('reviewed_at_commit', '?')}` are accepted and "
            f"unfixed: {named}. None is a disagreement; most need the experiment "
            f"re-run against the licensed feed. Dispositions for all {total}, "
            f"including the ones already fixed, are in "
            f"`{r.get('response', 'docs/paper/review/')}`.")


def half_spread_dollars(k, gamma):
    """The inventory-free half of A-S's spread: (1/gamma)*ln(1 + gamma/k).

    Half of the (2/gamma)*ln(1 + gamma/k) term in section 5, which is the part
    that does not depend on inventory or on the horizon -- so it is the floor
    on how tight the model will ever quote, and the thing to compare against
    the tick. gamma -> 0 takes it to 1/k, and the limit is taken here for the
    same reason the strategy takes it: the control arm is gamma = 0 and it has
    to stay inside the same code path."""
    if k is None or k <= 0:
        return None
    if gamma <= 0:
        return 1.0 / k
    return math.log(1.0 + gamma / k) / gamma


def no_conclusion_block():
    return "\n".join([
        "<!-- generated:conclusion:begin -->",
        "",
        "> **Not concluded.** `validation/as-experiment.json` is not committed, so there",
        "> is nothing to conclude from. This section is written by",
        "> `scripts/paper-report.py` out of the same artifact the tables in §7 come from,",
        "> and it emits a finding only when there is one — a conclusion is the easiest",
        "> place in a paper for a number to drift away from the run that produced it,",
        "> and the easiest sentence for a reader to quote.",
        ">",
        "> What §7 needs before this section can say anything is listed there.",
        "",
        "<!-- generated:conclusion:end -->",
    ])


def build_conclusion(d, extra, cals):
    if d is None:
        return no_conclusion_block()

    runs = d["runs"]
    gammas = d["gammas"]
    positive = [g for g in gammas if g > 0]
    gsel = positive[len(positive) // 2] if positive else 0.0
    symbols = d["symbols"]
    eval_days = d["evaluation_days"]
    k_by_symbol = d.get("k_by_symbol", {})

    cells = []
    for sym in symbols:
        for day in eval_days:
            for m in MODELS:
                t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                a = pick(runs, sym, day, "as", m, gsel)
                if t and z and a:
                    cells.append((sym, day, m, t, z, a))

    L = ["<!-- generated:conclusion:begin -->", ""]
    if not cells:
        L += ["> **Not concluded.** The committed artifact has no cell in which all "
              "three arms ran, so nothing here can be compared against anything.", "",
              "<!-- generated:conclusion:end -->"]
        return "\n".join(L)

    n = len(cells)

    # --- 8.1 the answer ---------------------------------------------------
    edge_worse = sum(1 for *_, t, _, a in cells
                     if a["edge_per_share_micros"] < t["edge_per_share_micros"])
    edge_neg_as = sum(1 for *_, a in cells if a["edge_per_share_micros"] < 0)
    edge_neg_t = sum(1 for *_, t, _, _ in cells if t["edge_per_share_micros"] < 0)
    inv_cut = statistics.median([(t["inv_max_abs"] - a["inv_max_abs"]) / t["inv_max_abs"]
                                 for *_, t, _, a in cells if t["inv_max_abs"] > 0])
    mk_worse = sum(1 for *_, t, _, a in cells if a["markout_1s"] < t["markout_1s"])
    mk_shift = statistics.median([a["markout_1s"] - t["markout_1s"]
                                  for *_, t, _, a in cells])

    d_edge = most(edge_worse, n)
    d_mk = most(mk_worse, n)
    verdict = {1: "no", -1: "yes"}.get(d_edge, "neither, on this sample")
    if d_edge == 1 and d_mk == 1:
        summary = ("Inventory-aware quoting in this implementation is not a cheaper "
                   "way to make the same market; it is a different market, made worse.")
    elif d_edge == -1 and d_mk == -1:
        summary = ("Inventory-aware quoting captured more half-spread and was no more "
                   "adversely selected on every cell here, which is the outcome the "
                   "pre-registration expected.")
    else:
        summary = ("Edge and markout do not point the same way across these cells, so "
                   "no single-sentence answer is stated: the per-symbol table in §8.2 "
                   "is the result, and this line is deliberately not a summary of it.")
    if inv_cut > 0:
        inv_para = (f"The inventory claim itself survives: median max\\|q\\| is "
                    f"**{inv_cut:.0%} below** the baseline's. A-S holds materially less "
                    f"inventory"
                    + (", and pays for it in adverse selection — the opposite of the "
                       "pre-registered mechanism (P2, §7.5)." if d_mk == 1 else
                       ", and does so without a markout penalty on most cells here."))
    else:
        inv_para = (f"The inventory claim does **not** survive on this sample: median "
                    f"max\\|q\\| is {abs(inv_cut):.0%} **above** the baseline's, so A-S "
                    f"carried larger excursions than the naive maker. P1 (§7.5) is "
                    f"graded on the same numbers and reads accordingly.")
    L += ["### 8.1 The answer",
          "",
          f"§1 asked whether inventory-aware quoting loses less than naive symmetric "
          f"quoting, and through which mechanism. Over "
          f"{plural(len(symbols), 'symbol')} ({', '.join(symbols)}), "
          f"{plural(len(eval_days), 'evaluation day')}, four fill models "
          f"and {n} symbol-day-lane cells at γ = {gsel:g}: **{verdict}**.",
          "",
          f"Half-spread captured per share is **lower than the naive touch maker's in "
          f"{edge_worse} of {n} cells**, and negative in {edge_neg_as} of {n} — against "
          f"{edge_neg_t} of {n} for the baseline. One-second markout is worse in "
          f"{mk_worse} of {n}, by a median of {abs(mk_shift):,.0f} micro-dollars per "
          f"share — a relative figure is avoided here because the baseline's markout "
          f"changes sign across symbols and a ratio across zero says nothing. "
          f"{summary}",
          ""]

    # The count at every swept gamma, not only the one the tables fix. Written
    # because a referee recounted it independently and it costs a sentence to
    # turn "we report gamma = X" into "the result does not depend on gamma".
    rc = gamma_recount(d)
    if len(rc) >= 2:
        held = [r for r in rc if r[2] == r[1]]
        negs = [r[3] for r in rc]
        gl = ", ".join(f"{g:g}" for g, *_ in rc)
        if len(held) == len(rc):
            L += [f"**That count does not depend on the γ the tables fix.** Recomputed "
                  f"at every swept γ ({gl}), A-S captures less half-spread than the "
                  f"touch maker in {rc[0][2]} of {rc[0][1]} cells at all "
                  f"{len(rc)} — only the number of outright *negative* cells moves, "
                  f"over {min(negs)} to {max(negs)}. γ = {gsel:g} is the middle of the "
                  f"swept grid by rule (§7), and on this axis the choice does not "
                  f"carry the finding.", ""]
        else:
            L += [f"**The count does depend on γ**, and the dependence is stated rather "
                  f"than hidden by the tables' choice: over the swept γ ({gl}) the "
                  f"cells where A-S captures less half-spread run "
                  f"{min(r[2] for r in rc)} to {max(r[2] for r in rc)} of "
                  f"{rc[0][1]}, and the outright negative cells {min(negs)} to "
                  f"{max(negs)}. A single γ is not the result.", ""]

    sd, ns = replication_units(d)
    L += [f"Every count out of {n} above shares the §1 caveat: {n} cells are four "
          f"lanes over {sd} symbol-days on {ns} stocks, and the lanes within a "
          f"symbol-day are not independent draws.", "",
          inv_para,
          ""]

    # --- 8.2 which half of A-S did it ------------------------------------
    L += ["### 8.2 Which half of A-S did it",
          "",
          "The three-arm design (§7) exists for this line. `as-gamma0` is A-S's spread "
          "with the inventory skew switched off, so the change from the baseline splits "
          "into a **spread choice** and a **skew**, and a two-arm comparison would have "
          "attributed all of it to inventory awareness.",
          "",
          "| symbol | edge/share: spread choice | edge/share: skew | max\\|q\\| vs "
          "baseline, skew **off** | …skew **on** |",
          "|---|---:|---:|---:|---:|"]
    for sym in symbols:
        c = [x for x in cells if x[0] == sym]
        if not c:
            continue
        spread = statistics.median([z["edge_per_share_micros"] - t["edge_per_share_micros"]
                                    for *_, t, z, _ in c])
        skew = statistics.median([a["edge_per_share_micros"] - z["edge_per_share_micros"]
                                  for *_, _, z, a in c])
        iz = statistics.median([(t["inv_max_abs"] - z["inv_max_abs"]) / t["inv_max_abs"]
                                for *_, t, z, _ in c if t["inv_max_abs"] > 0])
        ia = statistics.median([(t["inv_max_abs"] - a["inv_max_abs"]) / t["inv_max_abs"]
                                for *_, t, _, a in c if t["inv_max_abs"] > 0])
        L.append(f"| {sym} | {spread:+,.0f} | {skew:+,.0f} | {iz:+.0%} | {ia:+.0%} |")
    L += ["", "*Medians over that symbol's cells; edge in micro-dollars per share. A "
          "negative inventory column means A-S carried a **larger** excursion than the "
          "naive maker.*", ""]

    skew_helps = sum(1 for sym in symbols
                     for c in [[x for x in cells if x[0] == sym]] if c
                     and statistics.median([(t["inv_max_abs"] - a["inv_max_abs"]) /
                                            t["inv_max_abs"]
                                            for *_, t, _, a in c if t["inv_max_abs"] > 0]) > 0)
    skew_needed = sum(1 for sym in symbols
                      for c in [[x for x in cells if x[0] == sym]] if c
                      and statistics.median([(t["inv_max_abs"] - z["inv_max_abs"]) /
                                             t["inv_max_abs"]
                                             for *_, t, z, _ in c if t["inv_max_abs"] > 0]) < 0)
    spread_dominates = 0
    for sym in symbols:
        c = [x for x in cells if x[0] == sym]
        if not c:
            continue
        sp = abs(statistics.median([z["edge_per_share_micros"] - t["edge_per_share_micros"]
                                    for *_, t, z, _ in c]))
        sk = abs(statistics.median([a["edge_per_share_micros"] - z["edge_per_share_micros"]
                                    for *_, _, z, a in c]))
        if sp > sk:
            spread_dominates += 1
    ns = len(symbols)
    skew_line = (
        f"The **skew is a real and isolated win**: with it, A-S holds less inventory "
        f"than the naive maker on {skew_helps} of {ns}; without it, A-S holds *more* "
        f"on {skew_needed} of {ns}. Whatever the inventory result in §7.5 is, it "
        f"belongs to the skew and not to A-S's spread."
        if skew_helps > ns / 2 and skew_needed > 0 else
        f"The skew does not separate cleanly here: A-S holds less inventory than the "
        f"naive maker on {skew_helps} of {ns} symbols with the skew on, and on "
        f"{ns - skew_needed} of {ns} with it off, so the two arms are not telling "
        f"different stories about inventory on this sample.")
    spread_line = (
        f"And the **spread choice is where the money goes** — it is the larger of the "
        f"two columns on {spread_dominates} of {ns}, and it is the half the "
        f"pre-registration was not looking at."
        if spread_dominates > ns / 2 else
        f"The two columns are of comparable size: the spread choice is the larger on "
        f"only {spread_dominates} of {ns}, so this sample does not attribute the edge "
        f"change to one half of the model.")
    L += [f"Two things fall out of that table. {skew_line} {spread_line}", ""]

    # --- 8.3 the mechanism ------------------------------------------------
    L += ["### 8.3 The mechanism: the model's own half-spread against the tick",
          "",
          f"A-S's spread has an inventory-free floor, `(2/γ)·ln(1 + γ/k)` — half of it "
          f"below — and once k is *measured* rather than assumed, that floor is a "
          f"number the venue may not be able to express. The tick is "
          f"${TICK:.2f} (Reg NMS Rule 612), so half a tick is ${TICK / 2:.3f}.",
          "",
          "| symbol | median k (1/$) | half-spread floor | in ticks | fills, skew off, "
          "vs baseline | median edge/share, A-S |",
          "|---|---:|---:|---:|---:|---:|"]
    sign_match = 0
    sign_total = 0
    wide_floor = 0
    tighter_for_less = 0
    for sym in symbols:
        c = [x for x in cells if x[0] == sym]
        ks = [k for k in (k_by_symbol.get(sym) or {}).values() if k]
        if not c or not ks:
            continue
        kmed = statistics.median(ks)
        hs = statistics.median([h for k in ks
                                if (h := half_spread_dollars(k, gsel)) is not None])
        fill_ratio = statistics.median([z["fills"] / t["fills"]
                                        for *_, t, z, _ in c if t["fills"]])
        eq = statistics.median([a["edge_per_share_micros"] for *_, a in c])
        sign_total += 1
        edge_z = statistics.median([z["edge_per_share_micros"] for *_, z, _ in c])
        edge_t = statistics.median([t["edge_per_share_micros"] for *_, t, _, _ in c])
        if fill_ratio > 1.0 and edge_z < edge_t:
            tighter_for_less += 1
        if hs >= TICK / 2:
            wide_floor += 1
        if (hs < TICK / 2) == (eq < 0):
            sign_match += 1
        L.append(f"| {sym} | {kmed:,.1f} | ${hs:.5f} | {hs / TICK:.2f} | "
                 f"{fill_ratio:.2f}× | {eq:+,.0f} |")
    # BOTH outcomes must appear before the wording is allowed to firm up. A test
    # whose symbols all fall on one side of the line has not discriminated
    # anything, and "3 of 3" reads identically whether it did or not.
    discriminating = (sign_total > 0 and sign_match == sign_total
                      and 0 < wide_floor < sign_total)
    L += ["",
          (f"On {tighter_for_less} of {sign_total} symbols, switching the skew off and "
           f"quoting A-S's spread takes **more** fills than the naive touch maker at "
           f"**less** edge per share — the model quotes tighter than the touch, buys "
           f"volume with the half-spread it gives up, and is adversely selected for "
           f"the difference."
           if tighter_for_less > 0 else
           f"On this sample A-S's spread did not systematically trade edge for fills "
           f"against the touch maker, so the tick argument below has nothing to "
           f"explain and should be read as untested rather than as supported."),
          "",
          (f"The sign of A-S's captured edge is predicted by a single test — does the "
           f"inventory-free floor fit outside half a tick — on **{sign_match} of "
           f"{sign_total}** symbols."
           + ((" Where the floor has room on the price grid the edge stays positive; "
               "where it lands **below half a tick**, below the smallest increment the "
               "venue can quote, there is no price that expresses what the model wants "
               "and the edge goes negative. That is a mechanism, and it is falsifiable "
               "on the next symbol: a name whose measured k puts the floor several "
               "ticks wide should keep positive edge whatever its capitalisation, and "
               "one whose floor lands inside the tick should not.")
              if discriminating else
              (" A test that separates two cases has to have SEEN both: "
               f"{wide_floor} of {sign_total} symbols here put the floor outside half "
               f"a tick"
               + (", so every symbol falls on one side of the line and the test has "
                  "not been asked to discriminate."
                  if wide_floor in (0, sign_total) else
                  " and the test is not clean on all of them.")
               + " Read this as a hypothesis this sample is consistent with, not a "
                 "mechanism it establishes."))),
          ""]
    if discriminating:
        L += ["It is also a statement about **measurement**, not about "
              "Avellaneda–Stoikov. An implementation that assumes k — as almost all of "
              "them do — never discovers that its own spread formula is asking for a "
              "price the venue does not have. Assuming k is what hides this; measuring "
              "it is what shows it.", ""]

    # --- 8.4 identifiability ---------------------------------------------
    unfit = []
    kall = []
    for sym, cd in sorted(cals.items()):
        lanes = cd.get("lanes", {})
        bad = [ln for ln in MODELS if not lanes.get(ln, {}).get("fit_ok")]
        kall += [lanes[ln]["k"] for ln in MODELS
                 if lanes.get(ln, {}).get("fit_ok") and lanes[ln].get("k")]
        if bad:
            unfit.append((sym, len(bad), len(MODELS)))
    if kall:
        L += ["### 8.4 The second finding: how well λ(δ) can be identified at all",
              "",
              f"δ is measured from the mid, so the range of δ the strategy can *observe* "
              f"is bounded by the half-spread — and on a book that is one tick wide "
              f"there are only a handful of depth buckets with any exposure in them. "
              f"The fit does not fail cleanly at that end; it degrades, and it takes a "
              f"degrees-of-freedom count to see it. Across every calibrated symbol and "
              f"lane, fitted k spans **{min(kall):,.1f} to {max(kall):,.1f}**; a single "
              f"scalar over that range is not a parameter.",
              "",
              "| symbol | lanes fitted | buckets fitted (mbo lane) | residual dof | "
              "buckets with exposure and no fills |",
              "|---|---:|---:|---:|---:|"]
        for sym, cd in sorted(cals.items()):
            lanes = cd.get("lanes", {})
            fitted = sum(1 for ln in MODELS if lanes.get(ln, {}).get("fit_ok"))
            ref = lanes.get("mbo") or next((lanes[ln] for ln in MODELS
                                            if lanes.get(ln, {}).get("fit_ok")), {})
            L.append(f"| {sym} | {fitted}/{len(MODELS)} | "
                     f"{ref.get('buckets_fitted', 0)} | {ref.get('dof', 0)} | "
                     f"{ref.get('buckets_no_fills', 0)} |")
        L += ["", "A two-point fit has **zero** residual degrees of freedom and reports "
              "R² = 1.0000 whatever the data says, which is why `fit_ok` requires three "
              "points and not two. That guard is the only reason the row above with the "
              "fewest lanes reads as a refusal rather than as a perfect fit.", ""]
        if unfit:
            evaluated = [u for u in unfit if u[0] in symbols]
            L += ["Lanes with no usable fit, reported rather than dropped:", ""]
            for sym, bad, tot in unfit:
                where = "evaluated in §7" if sym in symbols else "not evaluated in §7"
                L.append(f"- **{sym}**: {bad} of {tot} lanes — {where}")
            L += ["",
                  ("The experiment driver refuses to run a lane with no fitted k unless "
                   "`--allow-assumed-k` is passed, which stamps the output "
                   "`assumed-scalar`. So a symbol in that state is not silently "
                   "evaluated on a placeholder — it is not evaluated."
                   if not evaluated else
                   "These lanes are evaluated below on an assumed rather than a fitted "
                   "k, and every table that uses them says so."),
                  "",
                  "A calibration that quietly emitted a number here would hand §5's "
                  "spread formula a curve fitted to nothing, and nothing downstream "
                  "would show it. The refusal is the finding.", ""]

    # --- 8.5 what remains -------------------------------------------------
    lat = sorted(extra) if extra else [d.get("latency_ns", 0)]
    lat_str = ", ".join(f"{x:,} ns" for x in lat)
    # P4 is graded in 7.5 the moment a second latency exists, so this item has
    # to stop asking for one -- a "what remains" list that keeps naming work
    # already done is the same stale-prose failure as the status banner was.
    # TWO POINTS IS NOT A LADDER, and this item called itself done on two. The
    # pre-registration asked for latency sensitivity "via the existing
    # latency_sweep", which is built on backtest.hpp and has no A-S arm and no
    # closed loop -- so the named tool could not grade P4 at all, and what ran
    # instead was two invocations of as_experiment. Saying "done" while the word
    # "sweep" stood in the prose is the deviation a referee named (M4); the
    # threshold below is where it stops being called done.
    if len(lat) >= 4:
        item1 = (f"1. **Latency sensitivity — done.** {len(lat)} modelled "
                 f"latencies ({lat_str}) are committed and P4 is graded in §7.5 "
                 f"against them. "
                 f"The verdict is not restated here; one copy of a graded prediction "
                 f"is the most this document keeps.")
    elif len(lat) >= 2:
        item1 = (f"1. **Latency sensitivity — graded on {len(lat)} points, which is "
                 f"not a sweep.** {len(lat)} modelled latencies ({lat_str}) are "
                 f"committed and P4 is graded against them in §7.5. Two endpoints "
                 f"establish a direction and not a shape, and the pre-registration "
                 f"asked for a ladder: `tools/latency_sweep.cpp` runs one, but on the "
                 f"open-loop `backtest.hpp` path with no A-S arm, so it cannot grade "
                 f"this prediction. The ladder here is N `as_experiment` runs under "
                 f"the `validation/as-experiment*.json` names this generator already "
                 f"globs, and it has not been run. The verdict is not restated here; "
                 f"one copy of a graded prediction is the most this document keeps.")
    else:
        item1 = (f"1. **Latency sensitivity.** {len(lat)} modelled latency "
                 f"({lat_str}) is committed, and P4 needs at least two to grade. The "
                 f"prediction — that A-S degrades faster than the baseline because it "
                 f"re-quotes more — is pre-registered and ungraded until a second "
                 f"`validation/as-experiment*.json` at a different `latency_ns` exists.")
    # THE OUTSIDE READER, GENERATED FROM THE REVIEW RATHER THAN ASSERTED ABOUT
    # IT. This item spent the whole phase as a hand-written "that has not
    # happened", which was true when typed and would have stayed on the page
    # unchanged the day it stopped being true -- the same stale-prose failure
    # the status banner exists to prevent, sitting inside the conclusion. It now
    # reads docs/paper/review/review.json, so the item can only say "reviewed"
    # while a review is committed, and says what KIND of review it was: an
    # unblinded reader who did not contest the construction is weaker evidence
    # than one who engaged with it and found it sound, and the difference is the
    # whole value of the item.
    L += ["### 8.5 What remains",
          "",
          item1,
          review_item(),
          f"3. **A second wide-floor name.** §8.3's test is carried by "
          f"{sign_total} symbols, of which {wide_floor} put the floor outside half a "
          f"tick. With {wide_floor} symbol{'' if wide_floor == 1 else 's'} on that side "
          f"of the line, \"the floor fits on the grid\" and \"this particular symbol\" "
          f"are not separated by this sample.",
          f"4. **More days.** {len(eval_days)} evaluation days is enough to report a "
          f"day-level spread and not enough to claim significance, and none is claimed "
          f"anywhere in this paper."]
    opens = open_review_item(5)
    if opens:
        L.append(opens)
    L += ["",
          "<!-- generated:conclusion:end -->"]
    return "\n".join(L)


# ------------------------------------------------------------------- plumbing

def splice(text, name, block):
    begin = f"<!-- generated:{name}:begin -->"
    end = f"<!-- generated:{name}:end -->"
    i = text.find(begin)
    j = text.find(end)
    if i < 0 or j < 0 or j < i:
        sys.exit(f"{PAPER}: missing or malformed {name} markers")
    return text[:i] + block + text[j + len(end):]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the paper is stale, and write nothing")
    a = ap.parse_args()

    if not PAPER.exists():
        sys.exit(f"missing {PAPER}")

    # Every as-experiment*.json is an experiment at one modelled latency; P4
    # needs at least two to grade. This is a naming convention rather than a new
    # schema: bench/as-experiment.py already takes --latency-ns and --out.
    extra = {}
    for p in sorted(glob.glob(EXPT_GLOB)):
        e = load(p)
        if e is not None and "runs" in e:
            extra[int(e.get("latency_ns", 0))] = e

    # RUN ORDER. This script links whichever figures exist, so running it before
    # scripts/paper-figures.sh writes a paper that omits figures which are about
    # to appear -- a document that is not visibly wrong, merely different from
    # what the same inputs produce next time. That happened once and --check
    # caught it. Use scripts/paper-build.sh, which runs the three in order; this
    # warning is the backstop for anyone who does not.
    if load(EXPT) is not None and load(MANIFEST) is None:
        print("WARNING: an evaluation artifact exists but no figure manifest does.\n"
              "         Run scripts/paper-figures.sh FIRST -- this paper will omit\n"
              "         figures that regenerating it later would include.\n"
              "         scripts/paper-build.sh runs all three in the right order.",
              file=sys.stderr)

    text = PAPER.read_text(encoding="utf-8")
    cals = load_calibrations()
    out = splice(text, "calibration", build_calibration(cals))
    out = splice(out, "results", build_results(load(EXPT), extra))
    out = splice(out, "findings", build_findings(load(EXPT)))
    out = splice(out, "conclusion", build_conclusion(load(EXPT), extra, cals))
    out = splice(out, "status", build_status(load(EXPT), cals))

    if a.check:
        if out != text:
            print(f"{PAPER.relative_to(ROOT)} is stale. "
                  f"Run: python3 scripts/paper-report.py", file=sys.stderr)
            return 1
        have = "with results" if load(EXPT) is not None else "results pending, stated as such"
        print(f"paper matches its artifacts ({have})")
        return 0

    PAPER.write_text(out, encoding="utf-8", newline="\n")
    print(f"wrote {PAPER.relative_to(ROOT)}"
          f"{'' if load(EXPT) is not None else ' (no evaluation artifact; results block says so)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
