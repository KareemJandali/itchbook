#!/usr/bin/env python3
"""as-experiment.py — the phase 11.3 matrix, with the rows kept.

Runs tools/as_experiment over every (symbol, day) pair and combines the results
WITHOUT pooling them. Every table this emits is per symbol-day; the only
aggregation is an explicit sensitivity table that shows the spread across days
rather than hiding it in a mean.

That is a rule from the plan and it has a reason: with a handful of symbol-days
there is no significance to claim, and an aggregate number invites exactly the
claim the data cannot support. A result driven entirely by one liquid name looks
identical to a property of the strategy once it has been averaged.

CALIBRATION AND EVALUATION NEVER SHARE A DAY. --calibration-day names the day k
was fitted on; that day is excluded from the evaluation tables and reported
separately. Passing a k measured on the same day being evaluated is the most
comfortable mistake available here, so the script refuses rather than warns.

Usage:
  bench/as-experiment.py --build build --out validation/as-experiment.json
      --feed MSFT:2019-12-30:data/sliced/MSFT-1230.gz
      --feed MSFT:2020-01-30:data/sliced/MSFT-0130.gz
      --calibration-day 2019-12-30 [--k 138] [--gammas 0.001,0.005,0.02,0.1]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

MODELS = ["naive", "optimistic", "mbo", "pessimistic"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--out", default="validation/as-experiment.json")
    ap.add_argument("--feed", action="append", default=[], metavar="SYMBOL:DAY:PATH",
                    help="repeatable; the symbol and day are recorded, not inferred")
    ap.add_argument("--calibration-day", required=True,
                    help="the day k was fitted on; excluded from evaluation")
    ap.add_argument("--k", type=float, default=200.0,
                    help="the MEASURED k from phase 11.2, not the assumed one")
    ap.add_argument("--gammas", default="0.001,0.005,0.02,0.1")
    ap.add_argument("--quote-size", type=int, default=100)
    ap.add_argument("--latency-ns", type=int, default=0)
    a = ap.parse_args()

    tool = os.path.join(os.path.abspath(a.build), "as_experiment")
    if not os.path.exists(tool):
        sys.exit(f"missing {tool}")
    if not a.feed:
        sys.exit("need at least one --feed SYMBOL:DAY:PATH")

    specs = []
    for spec in a.feed:
        parts = spec.split(":")
        if len(parts) != 3:
            sys.exit(f"--feed wants SYMBOL:DAY:PATH, got {spec!r}")
        symbol, day, path = parts
        if not os.path.exists(path):
            sys.exit(f"no such feed: {path}")
        specs.append((symbol, day, path))

    days = sorted({d for _, d, _ in specs})
    if a.calibration_day not in days:
        print(f"note: calibration day {a.calibration_day} is not among the feeds "
              f"({', '.join(days)}); nothing to exclude", file=sys.stderr)
    eval_days = [d for d in days if d != a.calibration_day]
    if not eval_days:
        sys.exit(f"every feed is on the calibration day ({a.calibration_day}). "
                 "Calibration and evaluation must not share a day — that is the "
                 "whole point of freezing k.")

    work = tempfile.mkdtemp(prefix="as-experiment-")
    runs = []
    for symbol, day, path in specs:
        out = os.path.join(work, f"{symbol}-{day}.json")
        cmd = [tool, path, "--symbol", symbol, "--day", day, "--gammas", a.gammas,
               "--k", str(a.k), "--quote-size", str(a.quote_size),
               "--latency-ns", str(a.latency_ns), "--json", out]
        print(f"==> {symbol} {day}")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{symbol} {day} failed:\n{r.stdout}\n{r.stderr}")
        d = json.load(open(out))
        for run in d["runs"]:
            run["symbol"] = symbol
            run["day"] = day
            run["is_calibration_day"] = (day == a.calibration_day)
            runs.append(run)

    result = {
        "k_measured": a.k,
        "gammas": [float(x) for x in a.gammas.split(",")],
        "calibration_day": a.calibration_day,
        "evaluation_days": eval_days,
        "quote_size": a.quote_size,
        "latency_ns": a.latency_ns,
        "symbols": sorted({s for s, _, _ in specs}),
        "runs": runs,
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    json.dump(result, open(a.out, "w"), indent=1)

    # ---- the tables, per symbol-day ---------------------------------------
    def pick(sym, day, arm, model, gamma=None):
        for r in runs:
            if (r["symbol"] == sym and r["day"] == day and r["arm"] == arm
                    and r["model"] == model
                    and (gamma is None or abs(r["gamma"] - gamma) < 1e-12)):
                return r
        return None

    best_gamma = result["gammas"][len(result["gammas"]) // 2]
    print(f"\n=== evaluation days only (calibration day {a.calibration_day} "
          f"excluded), gamma = {best_gamma:g} ===")
    print(f"{'symbol':<8} {'day':<12} {'model':<12} {'arm':<16} "
          f"{'eq/share':>10} {'inv sd':>10} {'inv max':>9} {'mk 1s':>9}")
    for sym in result["symbols"]:
        for day in eval_days:
            for model in MODELS:
                for arm, g in (("symmetric-touch", 0.0), ("as-gamma0", 0.0),
                               ("as", best_gamma)):
                    r = pick(sym, day, arm, model, g)
                    if r is None:
                        continue
                    print(f"{sym:<8} {day:<12} {model:<12} {arm:<16} "
                          f"{r['equity_per_share_micros']:>10} "
                          f"{r['inv_stdev']:>10.1f} {r['inv_max_abs']:>9} "
                          f"{r['markout_1s']:>9}")

    # ---- the mechanism decomposition, which is the phase's actual question --
    print("\n=== mechanism: which gap is which ===")
    print("touch -> gamma0 is the SPREAD choice; gamma0 -> as is the SKEW.")
    print(f"{'symbol':<8} {'day':<12} {'model':<12} {'d(eq) spread':>13} "
          f"{'d(eq) skew':>11} {'d(inv sd) spread':>17} {'d(inv sd) skew':>15}")
    for sym in result["symbols"]:
        for day in eval_days:
            for model in MODELS:
                t = pick(sym, day, "symmetric-touch", model, 0.0)
                z = pick(sym, day, "as-gamma0", model, 0.0)
                s = pick(sym, day, "as", model, best_gamma)
                if not (t and z and s):
                    continue
                print(f"{sym:<8} {day:<12} {model:<12} "
                      f"{z['equity_per_share_micros'] - t['equity_per_share_micros']:>13} "
                      f"{s['equity_per_share_micros'] - z['equity_per_share_micros']:>11} "
                      f"{z['inv_stdev'] - t['inv_stdev']:>17.1f} "
                      f"{s['inv_stdev'] - z['inv_stdev']:>15.1f}")

    # ---- day-level sensitivity, spread shown rather than averaged ----------
    print("\n=== day-level sensitivity (the spread IS the result; N is small) ===")
    print(f"{'symbol':<8} {'model':<12} {'arm':<16} {'days':>5} {'min eq/share':>13} "
          f"{'max eq/share':>13}")
    for sym in result["symbols"]:
        for model in MODELS:
            for arm, g in (("symmetric-touch", 0.0), ("as", best_gamma)):
                vals = [r["equity_per_share_micros"] for r in runs
                        if r["symbol"] == sym and r["model"] == model
                        and r["arm"] == arm and not r["is_calibration_day"]
                        and abs(r["gamma"] - g) < 1e-12]
                if not vals:
                    continue
                print(f"{sym:<8} {model:<12} {arm:<16} {len(vals):>5} "
                      f"{min(vals):>13} {max(vals):>13}")
    print("\nNo mean, and no significance claimed. With this many symbol-days the")
    print("spread between them is the honest summary and a mean would invite a")
    print("claim the data cannot support.")
    print(f"\nwrote {a.out}")


if __name__ == "__main__":
    main()
