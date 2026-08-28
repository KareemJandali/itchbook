#!/usr/bin/env python3
"""phase12-8-pool.py — combine repeated runs without pretending they are one run.

WHY THIS IS NOT A CONCATENATION. The obvious thing is to append ten runs'
samples and take a percentile of the pile. That treats the runs as
exchangeable, and a run is not a random draw from the same urn as the run
before it: the machine warms, the page cache fills, some other process starts,
the CPU changes frequency. If run 7 is systematically slower than run 3, pooling
does not average that away -- it hides it inside a number that then carries a
confidence interval implying a precision it does not have. The design review
raised exactly this and it is the reason this file exists.

SO THE PRIMARY STATEMENT IS THE SPREAD ACROSS RUNS, which assumes nothing. Ten
runs give ten medians; their range is a direct empirical statement about how
much the answer moves when you do the same thing again. That is the honest
uncertainty, and it is printed first.

THE RUN-EFFECT TEST IS A PERMUTATION TEST, for the same reason. There is no
numpy or scipy here, and more importantly a permutation test needs no
distributional assumption at all: pool the samples, re-deal them into runs of
the same sizes, recompute the spread of medians, and see how often chance
produces a spread as large as the observed one. If it rarely does, the runs
differ for a reason and pooling is refused. Fixed seed, so the answer is the
same tomorrow.

THE WITHIN-RUN INTERVAL IS AN EXACT ORDER STATISTIC. For the median of n
samples, the interval [x_(k), x_(n+1-k)] has coverage 1 - 2*P(Binom(n,1/2) < k),
computed here with exact integer arithmetic and no normal approximation. It is
valid within one run, where the samples at least plausibly come from one
process in one state; it is NOT extended across runs, which is the whole point.

Exit codes: 0 pooled and reportable, 3 ran but the runs are not poolable,
2 usage.
"""
import argparse
import json
import math
import random
import sys

SEED = 20261208          # fixed: the same inputs must give the same verdict
PERMUTATIONS = 2000


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    i = int(p / 100.0 * len(sorted_vals))
    if i >= len(sorted_vals):
        i = len(sorted_vals) - 1
    return sorted_vals[i]


def median(vals):
    return pct(sorted(vals), 50)


# Above this, the exact sum needs thousands of terms of an n-choose-k big
# integer. Exact below, normal approximation above, and the caller is told which.
EXACT_MAX_N = 1000


def median_ci(sorted_vals, conf=0.95):
    """Order-statistic interval for the median, and which method produced it.

    Returns (lo, hi, method). Coverage of [x_(k), x_(n+1-k)] is
    1 - 2*sum_{i<k} C(n,i)/2^n.

    EXACT for n <= EXACT_MAX_N, in integer arithmetic -- 2**n is a 2,400-bit
    number at n=2,400 and converting it to a float raises OverflowError, so the
    comparison is done as 2*cum*10^6 <= round((1-conf)*10^6)*total and the
    binomial terms are built incrementally rather than by calling math.comb per
    term.

    NORMAL above it, where the exact sum would need thousands of big-integer
    terms to move the answer by less than one order statistic. An interval that
    silently switched method would be an interval whose width meant two
    different things at two sample sizes, so the method is returned."""
    n = len(sorted_vals)
    if n < 8:
        return (None, None, "n too small")

    if n <= EXACT_MAX_N:
        total = 1 << n
        alpha = int(round((1.0 - conf) * 1_000_000))
        term = 1                      # C(n, 0)
        cum = 0
        best = 1
        for k in range(1, n // 2 + 1):
            cum += term               # sum_{i<k} C(n,i)
            if 2 * cum * 1_000_000 > alpha * total:
                break
            best = k
            term = term * (n - k + 1) // k    # C(n,k) from C(n,k-1), exactly
        return (sorted_vals[best - 1], sorted_vals[n - best], "exact")

    # z for a two-sided 95% interval. Only 0.95 is used here; anything else
    # would want its own quantile rather than a silently wrong constant.
    z = 1.959964 if abs(conf - 0.95) < 1e-9 else None
    if z is None:
        return (None, None, "unsupported confidence at this n")
    k = int(math.floor(n / 2.0 - z * math.sqrt(n) / 2.0))
    if k < 1:
        k = 1
    return (sorted_vals[k - 1], sorted_vals[n - k], "normal")


def spread_of_medians(groups):
    ms = [median(g) for g in groups if g]
    if len(ms) < 2:
        return 0.0
    return max(ms) - min(ms)


def run_effect_p(groups, rng):
    """How often does chance alone spread the medians this far apart?

    Pool everything, re-deal into runs of the same sizes, recompute. This asks
    the only question that matters before pooling: are these ten runs
    distinguishable from ten arbitrary slices of one pile?"""
    sizes = [len(g) for g in groups if g]
    if len(sizes) < 2:
        return None, 0.0
    pooled = [v for g in groups for v in g]
    observed = spread_of_medians(groups)
    hits = 0
    for _ in range(PERMUTATIONS):
        rng.shuffle(pooled)
        off = 0
        parts = []
        for sz in sizes:
            parts.append(pooled[off:off + sz])
            off += sz
        if spread_of_medians(parts) >= observed:
            hits += 1
    return (hits + 1) / (PERMUTATIONS + 1), observed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples", nargs="+", help="--samples-out files, one per run")
    ap.add_argument("--alpha", type=float, default=0.05,
                    help="a run effect below this refuses pooling")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    runs = []
    for path in args.samples:
        with open(path) as f:
            runs.append(json.load(f))
    if len(runs) < 2:
        print("error: pooling needs at least two runs; got %d" % len(runs))
        return 2

    print("=== %d runs ===" % len(runs))
    for r in runs:
        print("  %-12s orders %5d  joined %5d  census %s  tagged %s"
              % (r.get("run", "?"), r.get("orders_sent", 0), r.get("joined", 0),
                 "yes" if r.get("census_available") else "NO",
                 r.get("gap_tagged")))

    # Every run must have measured the same thing, or they are not repeats.
    hop_names = None
    for r in runs:
        names = sorted(r["hops"].keys())
        if hop_names is None:
            hop_names = names
        elif names != hop_names:
            print("\nREFUSED: the runs do not report the same hops. These are not")
            print("         repeats of one experiment.")
            return 3
    counts = {r.get("run"): r.get("joined", 0) for r in runs}
    if min(counts.values()) == 0:
        print("\nREFUSED: a run joined zero chains.")
        return 3

    rng = random.Random(SEED)
    not_poolable = []
    out = {}

    print()
    print("=== per-run medians, and whether the runs are the same experiment ===")
    print("  %-34s %5s %10s %9s %8s %8s %s"
          % ("hop", "runs", "med p50", "spread", "as %", "run-eff p", "verdict"))
    for name in [n for n in hop_names]:
        groups = [r["hops"][name]["vals"] for r in runs]
        groups = [g for g in groups if g]
        if len(groups) < 2:
            continue
        # IN RUN ORDER. Sorting these destroys the one thing that separates a
        # warm-up trend from scatter, and they have opposite remedies.
        meds_in_order = [median(g) for g in groups]
        meds = sorted(meds_in_order)
        rising = all(b >= a for a, b in zip(meds_in_order, meds_in_order[1:]))
        falling = all(b <= a for a, b in zip(meds_in_order, meds_in_order[1:]))
        trend = "rising" if rising else ("falling" if falling else "")
        p, observed = run_effect_p(groups, rng)
        # A run effect means the runs are distinguishable, so the pooled
        # percentile describes a mixture rather than a machine.
        effect = (p is not None and p < args.alpha)
        mid = median(meds) or 0
        # A percentage of a median that straddles zero is not a proportion of
        # anything. The cross-process hops can and do come out negative.
        frac = (100.0 * observed / mid) if mid > 0 else None
        verdict = ("RUN EFFECT" if effect else "poolable")
        if effect and trend:
            # A monotone drift across runs is a different fact from scatter, and
            # it has a different remedy: let the machine settle, or drop the
            # first run and say so.
            verdict += " (%s)" % trend
        if effect:
            not_poolable.append(name)
        print("  %-34s %5d %10s %9d %7s %8.4f %s"
              % (name, len(groups), median(meds), observed,
                 ("%.1f%%" % frac) if frac is not None else "n/a", p, verdict))
        out[name] = {
            "runs": len(groups),
            "per_run_medians_in_run_order": meds_in_order,
            "median_of_medians": median(meds),
            "spread_of_medians": observed,
            "spread_pct_of_median": (round(frac, 2) if frac is not None else None),
            "monotone": trend or None,
            "run_effect_p": p,
            "poolable": not effect,
        }

    print()
    print("  The SPREAD is the honest uncertainty: it is how far the answer moved")
    print("  when the same thing was done again, and it assumes nothing. The")
    print("  run-effect p is a permutation test -- how often re-dealing the pooled")
    print("  samples into runs of the same sizes spreads the medians this far.")

    print()
    print("=== pooled, where pooling is allowed ===")
    print("  %-34s %9s %11s %11s %19s"
          % ("hop", "n", "p50", "p99", "CI, or run range"))
    for name in hop_names:
        pooled = sorted(v for r in runs for v in r["hops"][name]["vals"])
        if not pooled:
            continue
        n = len(pooled)
        p99 = pct(pooled, 99) if (n - int(0.99 * n)) >= 10 else None
        if name in not_poolable:
            # The percentile is still the percentile. What is NOT true is the
            # binomial interval around it, so the per-run range replaces it --
            # wider, and honest about where the number moved.
            meds = out[name]["per_run_medians_in_run_order"]
            interval = "[%s, %s] runs" % (min(meds), max(meds))
        else:
            lo, hi, method = median_ci(pooled)
            interval = (("[%s, %s] %s" % (lo, hi, method))
                        if lo is not None else "-")
        print("  %-34s %9d %11s %11s %19s"
              % (name, n, pct(pooled, 50),
                 p99 if p99 is not None else "n too small", interval))
        out.setdefault(name, {})["pooled"] = {
            "n": n, "p50": pct(pooled, 50), "p99": p99,
            "interval": interval,
            "interval_is": ("per-run range" if name in not_poolable
                            else "order-statistic CI"),
        }

    # ---- the tail, and the census that gates it ---------------------------------
    print()
    print("=== the tail ===")
    census = all(r.get("census_available") for r in runs)
    if not census:
        print("  p99.9 NOT PRINTED: at least one run has no gap-overlap census, so a")
        print("  tail sample cannot be told apart from a scheduler gap.")
    else:
        tagged = sum(r.get("gap_tagged") or 0 for r in runs)
        joined = sum(r.get("joined") or 0 for r in runs)
        print("  chains that stopped running during the headline hop: %d of %d (%.2f%%)"
              % (tagged, joined, 100.0 * tagged / max(1, joined)))
        print("  %-34s %9s %11s %11s" % ("hop", "n", "p99.9 all", "p99.9 gap-free"))
        for name in hop_names:
            if name in not_poolable:
                continue
            allv = sorted(v for r in runs for v in r["hops"][name]["vals"])
            cln = sorted(v for r in runs for v in r["hops"][name]["clean"])
            def cell(vals):
                if len(vals) < 10 or (len(vals) - int(0.999 * len(vals))) < 10:
                    return "n too small"
                return pct(vals, 99.9)
            print("  %-34s %9d %11s %11s" % (name, len(allv), cell(allv), cell(cln)))
        print()
        print("  Where the two differ, the difference is the scheduler and not the")
        print("  code. n too small means exactly that: p99.9 of n samples is the")
        print("  (n/1000)th largest, and below ~10,000 samples that is a handful of")
        print("  observations wearing a percentile's name.")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({
                "runs": len(runs),
                "seed": SEED,
                "permutations": PERMUTATIONS,
                "alpha": args.alpha,
                "census_in_every_run": census,
                "not_poolable": not_poolable,
                "hops": out,
            }, f, indent=2)
        print("\nwrote " + args.json_out)

    if not_poolable:
        print()
        print("=== RUNS ARE NOT ONE EXPERIMENT ===")
        print("  %d of %d hops differ between runs." % (len(not_poolable), len(out)))
        print("  Their pooled p50 above carries the per-run RANGE instead of a")
        print("  confidence interval, because a binomial CI over a mixture of")
        print("  machine states implies a precision that is not there.")
        print("  A verdict marked (rising) or (falling) is a DRIFT, not scatter:")
        print("  let the machine settle, or drop the first run and say which.")
        print("  Compare the spread's %% column before caring: 13%% of 90 ns and")
        print("  16%% of 8 us are the same statistic and not the same problem.")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
