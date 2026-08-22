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
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAPER = ROOT / "docs" / "paper" / "as-on-itch.md"
CALIB = ROOT / "validation" / "intensity.json"
EXPT = ROOT / "validation" / "as-experiment.json"
EXPT_GLOB = str(ROOT / "validation" / "as-experiment*.json")
PHASE6 = ROOT / "docs" / "figures" / "touch-maker.json"
FIGREL = "../figures/paper"
FIGDIR = ROOT / "docs" / "figures" / "paper"
MANIFEST = FIGDIR / "manifest.json"

MODELS = ["naive", "optimistic", "mbo", "pessimistic"]
ARMS = ["symmetric-touch", "as-gamma0", "as"]

# The pre-registered scope, from the Phase 11 done-list in the plan.
MIN_SYMBOLS = 3
MIN_EVAL_DAYS = 3

# The bars, transcribed once from docs/build-plan-9-12.md 11.3. Nothing below
# writes a verdict; every verdict is computed against these.
P1_INVENTORY_CUT = 0.30     # max|q| lower by at least 30%
P2_MARKOUT_EQUIV = 0.10     # within 10% of baseline counts as indistinguishable
P3_NEGATIVE_LANES = 3       # equity/share negative in >= 3 of 4 lanes
MAJORITY = 0.5              # "on most symbol-days"


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


def load(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------- calibration

def build_calibration(d):
    L = ["<!-- generated:calibration:begin -->", ""]
    if d is None:
        L += ["> **Not measured.** `validation/intensity.json` is not committed, so "
              "no fitted A or k appears here. The calibration runs with:",
              ">",
              "> ```",
              "> build/calibrate_intensity data/sliced/SYM-DAY.gz \\",
              ">     --json validation/intensity.json",
              "> ```",
              ">",
              "> Until it does, §5's spread formula is being fed the **default** k "
              "rather than a measured one — the paper says so rather than printing "
              "a number it does not have, and §6.1's per-lane decision has nothing "
              "to be per-lane about yet.", "",
              "<!-- generated:calibration:end -->"]
        return "\n".join(L)

    lanes = d.get("lanes", {})
    L += [f"Fitted per lane (`calibrated_per_lane`: "
          f"`{str(d.get('calibrated_per_lane', False)).lower()}`), which is the §6.1 "
          f"decision made visible in the artifact rather than in prose.", "",
          "| lane | A | k (1/$) | R² | buckets fitted | buckets with exposure, no fills | "
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

    fitted = {m: lanes[m] for m in MODELS if m in lanes and lanes[m].get("fit_ok")}
    if fitted:
        ks = {m: c["k"] for m, c in fitted.items()}
        lo, hi = min(ks.values()), max(ks.values())
        L += ["", f"k ranges from {lo:.1f} to {hi:.1f} across the four lanes — a spread of "
                  f"{(hi / lo if lo else float('nan')):.2f}× on the parameter that sets the "
                  f"spread. That spread is the §6.1 cost stated as a number: calibrating once "
                  f"and evaluating four times would have handed three lanes a k that is wrong "
                  f"by up to that factor."]
        drops = sum(c["buckets_no_fills"] for c in fitted.values())
        if drops:
            L += ["", f"{drops} bucket(s) across the four lanes had exposure and no fills. "
                      f"They cannot be logged and are excluded from the fit; they are "
                      f"reported because dropping them silently flattens the curve — the deep "
                      f"buckets are exactly the ones that fail to fill."]

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
        ">",
        "> ```",
        "> bench/as-experiment.py --build build --out validation/as-experiment.json \\",
        ">     --feed SYM:YYYY-MM-DD:data/sliced/SYM-DAY.gz [--feed ...] \\",
        ">     --calibration-day YYYY-MM-DD --k <the measured k, not the assumed one>",
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
    L += [f"{len(symbols)} symbol(s) — {', '.join(symbols)} — over "
          f"{len(eval_days)} evaluation day(s), with {d['calibration_day']} held out as "
          f"the calibration day. k = {d['k_measured']:.1f}, quote size "
          f"{d['quote_size']}, modelled latency {d['latency_ns']:,} ns. "
          f"γ is swept over {', '.join(f'{g:g}' for g in gammas)}; the tables below "
          f"fix γ = {gsel:g} and the sweep itself is the figure.", ""]
    if not ok_scope:
        L += [f"> **Below the pre-registered scope.** The plan's done-list requires "
              f"≥ {MIN_SYMBOLS} symbols × ≥ {MIN_EVAL_DAYS} evaluation days; this "
              f"artifact has {len(symbols)} × {len(eval_days)}. The tables are printed "
              f"because they are what was measured, and the predictions below are graded "
              f"against them, but **the phase's done-condition is not met** and no "
              f"conclusion here should be read as though it were.", ""]

    # --- 7.1 headline, per symbol-day ------------------------------------
    L += ["### 7.1 Headline band, per symbol-day", "",
          "Equity is µ$ per share after fees. `max |q|` is the largest absolute "
          "position held. `mk 1s` is the 1-second markout per share — negative is "
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
                  "| lane | eq touch | eq γ=0 | eq A-S | max\\|q\\| touch | max\\|q\\| A-S | "
                  "mk 1s touch | mk 1s A-S |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
            for m, t, z, s in rows:
                L.append(f"| {m} | {t['equity_per_share_micros']:,} | "
                         f"{z['equity_per_share_micros']:,} | "
                         f"{s['equity_per_share_micros']:,} | "
                         f"{t['inv_max_abs']:,} | {s['inv_max_abs']:,} | "
                         f"{t['markout_1s']:,} | {s['markout_1s']:,} |")
            band_t = rel_band([t["equity_per_share_micros"] for _, t, _, _ in rows])
            band_s = rel_band([s["equity_per_share_micros"] for _, _, _, s in rows])
            if band_t is not None and band_s is not None:
                L += ["", f"Band width (max − min over the four lanes, scaled by the median "
                          f"|equity|): touch-maker {band_t:.2f}, A-S {band_s:.2f}."]
            L.append("")

    # --- 7.2 mechanism ----------------------------------------------------
    L += ["### 7.2 Mechanism: which gap is which", "",
          "`touch → γ=0` is the **spread choice**; `γ=0 → A-S` is the **inventory "
          "skew**. A two-arm comparison bundles them, and the bundled number is what "
          "gets reported as \"A-S wins\". Δ is A-S-side minus baseline-side.", "",
          "| symbol | day | lane | Δeq spread | Δeq skew | Δinv sd spread | Δinv sd skew |",
          "|---|---|---|---:|---:|---:|---:|"]
    for sym in symbols:
        for day in eval_days:
            for m in MODELS:
                t = pick(runs, sym, day, "symmetric-touch", m, 0.0)
                z = pick(runs, sym, day, "as-gamma0", m, 0.0)
                s = pick(runs, sym, day, "as", m, gsel)
                if not (t and z and s):
                    continue
                L.append(f"| {sym} | {day} | {m} | "
                         f"{z['equity_per_share_micros'] - t['equity_per_share_micros']:,} | "
                         f"{s['equity_per_share_micros'] - z['equity_per_share_micros']:,} | "
                         f"{z['inv_stdev'] - t['inv_stdev']:,.1f} | "
                         f"{s['inv_stdev'] - z['inv_stdev']:,.1f} |")
    L.append("")

    # --- 7.3 day-level spread --------------------------------------------
    L += ["### 7.3 Day-level spread", "",
          "The spread across days **is** the result. No mean is taken: with this many "
          "symbol-days a mean invites a claim the data cannot support.", "",
          "| symbol | lane | arm | days | min eq/share | max eq/share |",
          "|---|---|---|---:|---:|---:|"]
    for sym in symbols:
        for m in MODELS:
            for arm, g in (("symmetric-touch", 0.0), ("as", gsel)):
                vals = [r["equity_per_share_micros"] for r in runs
                        if r["symbol"] == sym and r["model"] == m and r["arm"] == arm
                        and not r["is_calibration_day"] and abs(r["gamma"] - g) < 1e-12]
                if not vals:
                    continue
                L.append(f"| {sym} | {m} | {arm} | {len(vals)} | {min(vals):,} | "
                         f"{max(vals):,} |")
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

    text = PAPER.read_text()
    out = splice(text, "calibration", build_calibration(load(CALIB)))
    out = splice(out, "results", build_results(load(EXPT), extra))

    if a.check:
        if out != text:
            print(f"{PAPER.relative_to(ROOT)} is stale. "
                  f"Run: python3 scripts/paper-report.py", file=sys.stderr)
            return 1
        have = "with results" if load(EXPT) is not None else "results pending, stated as such"
        print(f"paper matches its artifacts ({have})")
        return 0

    PAPER.write_text(out)
    print(f"wrote {PAPER.relative_to(ROOT)}"
          f"{'' if load(EXPT) is not None else ' (no evaluation artifact; results block says so)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
