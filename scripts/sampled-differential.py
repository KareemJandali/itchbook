#!/usr/bin/env python3
"""9.12's sampled differential: K symbols, seeded, oracle vs C++, bit-identical.

    python3 scripts/sampled-differential.py <full-day.gz> \
        --symbols-from validation/all-symbols-2019-12-30.csv --seed 20261230

The oracle cannot chew 268M messages -- its job description is "slow, obvious,
correct". The plan's answer is to sample: seeded random selection of K symbols,
always including MSFT and one ETF, each sliced out and run through both
implementations, each required to be bit-identical in BOTH the snapshot CSV and
the daily summary. The seed is printed and recorded so a run reproduces.

Why seeded rather than chosen: a differential over symbols someone picked is a
differential over symbols someone picked. The seed is what makes "eight symbols
agreed" a claim about the book rather than about the author's taste, and it is
recorded in the artifact so the same eight can be demanded again.

Why MSFT and an ETF are pinned anyway: a uniform sample of 8,906 NASDAQ
securities is overwhelmingly small symbols, and a day's worth of a mega-cap
exercises paths -- deep books, heavy replace traffic, a crossed auction with
real size -- that a thinly quoted name never reaches. The pins are named in the
artifact so nobody has to guess which rows were sampled and which were not.

Exits 0 only if every symbol matched on both comparisons.
"""
import argparse
import csv
import json
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python" / "analysis"))
import summary_diff  # noqa: E402  (after sys.path)

# The ETF is picked by name, not by flag: the stock directory's ETP field is not
# parsed anywhere in this repo, so there is nothing to filter on. First of these
# that quoted on the day is used, and which one is recorded in the artifact.
ETF_CANDIDATES = ("QQQ", "SPY", "IWM", "TQQQ")
PINNED = ("MSFT",)


def pick(rows, k, seed):
    """(symbols, etf, pool_size) -- pinned names first, then a seeded sample."""
    quoted = sorted(r["symbol"] for r in rows
                    if r["directoried"] == "yes" and int(r["adds"]) > 0)
    have = set(quoted)
    chosen = [s for s in PINNED if s in have]
    etf = next((e for e in ETF_CANDIDATES if e in have), None)
    if etf and etf not in chosen:
        chosen.append(etf)
    # Sample from what is left, so a pin can never be drawn twice and the
    # sample size does not silently shrink when it would have been.
    pool = [s for s in quoted if s not in chosen]
    rng = random.Random(seed)
    chosen += rng.sample(pool, max(0, k - len(chosen)))
    return chosen, etf, len(quoted)


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed")
    ap.add_argument("--symbols-from", required=True,
                    help="per-symbol CSV from book_replay --all-symbols")
    ap.add_argument("--k", type=int, default=8, help="symbols in total (>= 8)")
    ap.add_argument("--seed", type=int, default=None,
                    help="omit to rotate; CI pins one")
    ap.add_argument("--build", default="build")
    ap.add_argument("--out", default="out/sampled-differential")
    ap.add_argument("--slices", default="data/sliced")
    ap.add_argument("--interval-ms", type=int, default=1000)
    ap.add_argument("--levels", type=int, default=10)
    ap.add_argument("--json", dest="artifact", default=None)
    args = ap.parse_args()

    if args.k < 8:
        sys.exit("error: 9.12 requires K >= 8")
    seed = args.seed if args.seed is not None else random.randrange(2**31)
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    slices = Path(args.slices); slices.mkdir(parents=True, exist_ok=True)
    binp = Path(args.build)

    rows = list(csv.DictReader(open(args.symbols_from)))
    symbols, etf, pool = pick(rows, args.k, seed)

    print(f"seed {seed}  |  {len(symbols)} symbols from a pool of {pool:,} that quoted")
    print(f"pinned: {', '.join(PINNED)}" + (f" + {etf} (ETF)" if etf else " (no ETF found)"))
    print(f"sampled: {', '.join(s for s in symbols if s not in PINNED and s != etf)}")
    print()

    results, failed = [], 0
    for i, sym in enumerate(symbols, 1):
        t0 = time.time()
        sl = slices / f"{sym}.gz"
        if not sl.exists():
            run([str(binp / "itch_slice"), args.feed, sym, str(sl)])
        py_csv, cpp_csv = out / f"{sym}.py.csv", out / f"{sym}.cpp.csv"
        py_js, cpp_js = out / f"{sym}.py.json", out / f"{sym}.cpp.json"
        run([sys.executable, str(ROOT / "python/reference/replay.py"), str(sl),
             "--snapshots", str(py_csv), "--interval-ms", str(args.interval_ms),
             "--levels", str(args.levels), "--json", str(py_js), "--quiet"])
        run([str(binp / "book_replay"), str(sl),
             "--snapshots", str(cpp_csv), "--interval-ms", str(args.interval_ms),
             "--levels", str(args.levels), "--json", str(cpp_js), "--quiet"])

        snaps = subprocess.run([sys.executable, str(ROOT / "python/analysis/book_diff.py"),
                                str(py_csv), str(cpp_csv)], capture_output=True, text=True)
        rows_compared = sum(1 for _ in open(py_csv)) - 1
        keys, bad = summary_diff.report(json.load(open(py_js)), json.load(open(cpp_js)),
                                        quiet=True)
        # Two files that are both empty are identical, and that is not a pass.
        # The first run of this script reported eight symbols "bit-identical"
        # over ZERO snapshot rows, because the sampling interval was longer than
        # the synthetic session and every CSV came out as a header line. An
        # agreement nothing was compared for is the failure mode this whole
        # repository is about, so it is named here rather than counted.
        empty = rows_compared == 0 or len(keys) == 0
        ok = snaps.returncode == 0 and not bad and not empty
        failed += 0 if ok else 1
        results.append({"symbol": sym, "pinned": sym in PINNED or sym == etf,
                        "snapshot_rows": rows_compared, "summary_fields": len(keys),
                        "snapshots_identical": snaps.returncode == 0,
                        "summary_fields_differing": bad,
                        "compared_nothing": empty, "ok": ok})
        mark = "ok  " if ok else ("VOID" if empty else "FAIL")
        print(f"  {mark} {sym:<8} {rows_compared:>6,} snapshot rows, "
              f"{len(keys):>2} summary fields, {time.time()-t0:5.1f}s")
        if empty:
            print(f"       compared nothing: {rows_compared} snapshot rows, "
                  f"{len(keys)} summary fields. The interval "
                  f"({args.interval_ms} ms) is longer than this symbol's session, "
                  "or the slice is empty. Not a pass.")
        elif not ok:
            print(snaps.stdout or snaps.stderr)
            if bad:
                print(f"       summary disagrees on: {', '.join(bad)}")

    print()
    total_rows = sum(r["snapshot_rows"] for r in results)
    void = sum(1 for r in results if r["compared_nothing"])
    if failed:
        disagree = failed - void
        parts = []
        if disagree:
            parts.append(f"{disagree} disagree")
        if void:
            parts.append(f"{void} compared nothing")
        print(f"FAIL: {failed} of {len(results)} symbols -- " + ", ".join(parts))
    else:
        print(f"OK: {len(results)} symbols bit-identical to the oracle across "
              f"{total_rows:,} snapshot rows and "
              f"{sum(r['summary_fields'] for r in results):,} summary fields "
              f"(seed {seed})")

    if args.artifact:
        Path(args.artifact).write_text(json.dumps({
            "feed": args.feed, "seed": seed, "k": args.k,
            "pinned": list(PINNED), "etf": etf, "pool_size": pool,
            "interval_ms": args.interval_ms, "levels": args.levels,
            "symbols": results,
            "total_snapshot_rows": total_rows,
            "symbols_comparing_nothing": void,
            "all_identical": failed == 0,
        }, indent=2) + "\n")
        print(f"wrote {args.artifact}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
