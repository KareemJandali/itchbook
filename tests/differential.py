#!/usr/bin/env python3
"""Differential test: C++ book_replay vs the Python oracle — the phase 3 gate.

Generates adversarial feeds with `fuzz_feed.py`, replays each through both
implementations, and requires the snapshot CSVs and the printed summaries to
agree byte for byte. Any divergence is a bug in one of them, and the oracle is
presumed right until shown otherwise.

    python3 tests/differential.py --binary build/book_replay --seeds 5

This is not a unit test; it needs a built binary, so CI runs it as its own step
after the build rather than under `unittest discover`.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def one_seed(binary, seed, messages, levels, interval_ms, workdir):
    feed = workdir / f"fuzz{seed}.gz"
    py_csv = workdir / f"py{seed}.csv"
    cpp_csv = workdir / f"cpp{seed}.csv"

    run([sys.executable, str(ROOT / "python" / "fuzz_feed.py"), str(feed),
         "--seed", str(seed), "--messages", str(messages)])

    py = run([sys.executable, str(ROOT / "python" / "reference" / "replay.py"), str(feed),
              "--symbol", "TEST", "--snapshots", str(py_csv),
              "--interval-ms", str(interval_ms), "--levels", str(levels)])
    cpp = run([str(binary), str(feed), "--symbol", "TEST", "--snapshots", str(cpp_csv),
               "--interval-ms", str(interval_ms), "--levels", str(levels)])

    diff = subprocess.run(
        [sys.executable, str(ROOT / "python" / "analysis" / "book_diff.py"),
         str(py_csv), str(cpp_csv)],
        capture_output=True, text=True)
    ok = diff.returncode == 0

    # The summaries must match too, apart from the line naming the output file.
    def strip(text):
        return [ln for ln in text.splitlines() if "snapshots written" not in ln]

    py_sum, cpp_sum = strip(py.stdout), strip(cpp.stdout)
    summary_ok = py_sum == cpp_sum

    rows = sum(1 for _ in py_csv.open()) - 1
    print(f"seed {seed}: {diff.stdout.strip()}")
    if not summary_ok:
        print("  SUMMARY MISMATCH")
        for a, b in zip(py_sum, cpp_sum):
            if a != b:
                print(f"    python: {a!r}")
                print(f"    c++   : {b!r}")
    if not ok:
        print(diff.stdout)
    return ok and summary_ok, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default="build/book_replay")
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--messages", type=int, default=20_000)
    ap.add_argument("--levels", type=int, default=10)
    ap.add_argument("--interval-ms", type=float, default=0.05)
    a = ap.parse_args()

    binary = Path(a.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"error: {binary} not found — build first")

    failures = 0
    total_rows = 0
    with tempfile.TemporaryDirectory() as d:
        for seed in range(1, a.seeds + 1):
            ok, rows = one_seed(binary, seed, a.messages, a.levels, a.interval_ms, Path(d))
            total_rows += rows
            if not ok:
                failures += 1

    print()
    if failures:
        print(f"FAIL: {failures}/{a.seeds} seeds diverged")
        return 1
    print(f"OK: {a.seeds} seeds, {total_rows:,} snapshot rows, "
          f"C++ and the oracle agree byte for byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
