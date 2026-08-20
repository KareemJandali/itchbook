#!/usr/bin/env python3
"""Run book_bench repeatedly and compare two builds honestly.

Written because the first round of phase 4 measurements were wrong. Run
unpinned on a shared machine, book_bench varies by ~20% between identical
invocations — enough to manufacture a convincing 12% "speedup" out of nothing,
which is exactly what happened before this script existed.

Two rules it enforces:

  * **Pin to one core.** Pinning takes run-to-run spread from ~19% to ~2%. The
    noise was the scheduler migrating the process, not the code.

  * **Interleave the variants.** Machine state drifts over minutes. Running all
    of A then all of B attributes that drift to the change under test; A, B, A,
    B, ... does not.

    python3 bench/compare.py ./build/book_bench data/raw/bench.gz
    python3 bench/compare.py ./old_bench ./new_bench data/raw/bench.gz --rounds 9

Reports medians, not means: one descheduled run has no business moving the
answer.
"""
import argparse
import re
import shutil
import statistics as st
import subprocess
import sys
from pathlib import Path

CYCLES = re.compile(r"([\d.]+) cycles/msg")
ROW = re.compile(r"^all\s+\d+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", re.M)


def run_once(binary, feed, cpu, extra):
    cmd = []
    if cpu is not None and shutil.which("taskset"):
        cmd += ["taskset", "-c", str(cpu)]
    cmd += [str(binary), str(feed), *extra]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"error: {binary} failed\n{out.stderr}")
    cyc = CYCLES.search(out.stdout)
    row = ROW.search(out.stdout)
    if cyc is None or row is None:
        raise SystemExit(f"error: could not parse book_bench output:\n{out.stdout}")
    return {"cycles": float(cyc.group(1)), "p50": int(row.group(1)),
            "p99": int(row.group(2)), "p999": int(row.group(3)), "max": int(row.group(4))}


def summarise(label, runs):
    cyc = [r["cycles"] for r in runs]
    spread = (max(cyc) - min(cyc)) / st.median(cyc) * 100
    print(f"{label:<26} {st.median(cyc):10.2f} {st.median([r['p50'] for r in runs]):7.0f} "
          f"{st.median([r['p99'] for r in runs]):7.0f} "
          f"{st.median([r['p999'] for r in runs]):8.0f} "
          f"{st.median([r['max'] for r in runs]):12.0f} {spread:8.1f}%")
    return st.median(cyc), spread


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("args", nargs="+",
                    help="<binary> <feed> or <binary_a> <binary_b> <feed>")
    ap.add_argument("--rounds", type=int, default=9)
    ap.add_argument("--cpu", type=int, default=2, help="core to pin to; -1 to disable")
    ap.add_argument("--extra", nargs="*", default=[], help="extra args for book_bench")
    a = ap.parse_args()

    cpu = None if a.cpu < 0 else a.cpu
    if len(a.args) == 2:
        variants = [("current", Path(a.args[0]))]
        feed = a.args[1]
    elif len(a.args) == 3:
        variants = [("A: " + Path(a.args[0]).name, Path(a.args[0])),
                    ("B: " + Path(a.args[1]).name, Path(a.args[1]))]
        feed = a.args[2]
    else:
        ap.error("pass either <binary> <feed> or <binary_a> <binary_b> <feed>")

    for _, b in variants:
        if not Path(b).exists():
            raise SystemExit(f"error: {b} not found — build it first")

    if cpu is not None and not shutil.which("taskset"):
        print("warning: taskset not available; expect ~10x more run-to-run noise\n")

    results = {label: [] for label, _ in variants}
    for _ in range(a.rounds):
        for label, binary in variants:     # interleaved, never A-then-B
            results[label].append(run_once(binary, feed, cpu, a.extra))

    print(f"\n{a.rounds} rounds, interleaved"
          f"{f', pinned to cpu {cpu}' if cpu is not None else ', unpinned'}\n")
    print(f"{'variant':<26} {'cycles/msg':>10} {'p50':>7} {'p99':>7} {'p999':>8} "
          f"{'max':>12} {'spread':>9}")
    print("-" * 84)
    medians = [summarise(label, results[label]) for label, _ in variants]
    print("-" * 84)

    for _, spread in medians:
        if spread > 5.0:
            print(f"\nwarning: {spread:.1f}% spread — too noisy to trust a small delta.")
            print("         Pin to a core, close other work, or raise --rounds.")
            break

    if len(medians) == 2:
        a_med, b_med = medians[0][0], medians[1][0]
        delta = (a_med - b_med) / a_med * 100
        print(f"\nB vs A: {a_med / b_med:.2f}x   ({delta:+.1f}% cycles per message)")
        if abs(delta) < 5.0:
            print("        Under 5% on a machine like this is not a result.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
