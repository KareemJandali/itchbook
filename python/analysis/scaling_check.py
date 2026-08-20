#!/usr/bin/env python3
"""Assert the backtester's cost is linear in the length of the feed.

    python3 python/analysis/scaling_check.py --build build

Two quadratics lived in the simulator from the moment it was written until real
data exposed them, and both were invisible at the size this repo tests at. A
six-point latency sweep over a real 1.2M-message day took the better part of an
hour; it takes four seconds now. Nothing in the test suite noticed, because
every one of those tests is about correctness and a quadratic is not wrong, only
ruinous.

The check has to be machine-independent, which rules out a wall-clock ceiling:
CI runners vary by more than the margin worth alerting on, and a threshold loose
enough never to false-alarm is loose enough to miss the thing. So it measures
the RATIO between two feed sizes instead. Double the input and:

    linear      ~2x the time
    quadratic   ~4x the time

and the machine's speed cancels out of the ratio entirely. The historical
numbers make the separation concrete — the pre-fix code measured 1.5s / 5.5s /
21.2s at 50k / 100k / 200k messages, ratios of 3.6 and 3.9. A threshold of 3.0
sits clear of both a healthy 2.0 and a broken 3.6.

Timing is noisy, so each size runs several times and the FASTEST is kept.
A minimum is the right statistic here: interference from other processes can
only ever make a run slower, so the fastest observation is the closest to the
work actually being done.
"""
import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def make_feed(path, messages, seed):
    subprocess.run([sys.executable, "python/make_queue_feed.py", str(path),
                    "--messages", str(messages), "--gap-ns", "200000",
                    "--seed", str(seed)],
                   check=True, capture_output=True)


def time_run(cmd, repeats):
    best = None
    for _ in range(repeats):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True)
        dt = time.perf_counter() - t0
        if r.returncode != 0:
            raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n"
                             f"{r.stderr.decode()[:400]}")
        best = dt if best is None else min(best, dt)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--messages", type=int, default=200000,
                    help="the smaller feed; the larger is twice this")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--max-ratio", type=float, default=3.0,
                    help="fail above this. linear is ~2, quadratic ~4")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()

    tool = Path(a.build) / "queue_backtest"
    if not tool.exists():
        raise SystemExit(f"error: {tool} not found — build first")

    with tempfile.TemporaryDirectory() as tmp:
        small = Path(tmp) / "small.gz"
        large = Path(tmp) / "large.gz"
        print(f"generating {a.messages:,} and {a.messages * 2:,} message feeds")
        make_feed(small, a.messages, a.seed)
        make_feed(large, a.messages * 2, a.seed)

        # touch-maker on purpose: it requotes on every touch move, so it is the
        # strategy that exercises the per-quote and per-fill paths hardest. A
        # strategy that rarely acts would have hidden both historical bugs.
        def cmd(feed):
            return [str(tool), str(feed), "--strategy", "touch-maker",
                    "--max-position", "1000"]

        t_small = time_run(cmd(small), a.repeats)
        t_large = time_run(cmd(large), a.repeats)

    ratio = t_large / t_small if t_small > 0 else float("inf")
    print(f"\n{a.messages:>9,} messages: {t_small * 1000:8.1f} ms")
    print(f"{a.messages * 2:>9,} messages: {t_large * 1000:8.1f} ms")
    print(f"ratio: {ratio:.2f}x for 2x the input "
          f"(linear ~2.0, quadratic ~4.0, limit {a.max_ratio})")

    if ratio > a.max_ratio:
        print("\nFAIL: cost is growing faster than the feed.")
        print("Something on the per-message path is walking a structure that")
        print("grows with the run. The last two were markout samples awaiting a")
        print("horizon, and dead entries never erased from the quote list.")
        return 1
    print("\nOK: linear in the length of the feed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
