#!/usr/bin/env python3
"""Differential test: C++ queue_sim against the Python queue oracle.

The same discipline that closed phases 2 and 3. Two independent implementations
of the same rules, run over identical bytes, required to produce byte-identical
fill logs. A queue model is a counterfactual — there is no ground truth to check
it against — so agreement between two implementations written from the same spec
is most of the assurance available before real data.

Feeds are swept across `--cancel-front-bias`, because that is the parameter the
models actually differ on: at 1.0 every cancel is ahead of everyone and the
optimistic model is right; at 0.0 they are all behind and the pessimistic one is.
A differential run at a single bias would leave most of the disagreement
untested.

    python3 tests/queue_differential.py --binary build/queue_sim
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

from reference import queue_sim   # noqa: E402


def run_cpp(binary, feed, placements, out_csv):
    cmd = [str(binary), str(feed)]
    for side, price, qty, after, display in placements:
        spec = f"{side}:{price}:{qty}:{after}"
        if display:
            spec += f":{display}"
        cmd += ["--place", spec]
    cmd += ["--fills", str(out_csv)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"queue_sim failed:\n{r.stderr}")
    return out_csv.read_text()


def run_python(feed, placements):
    lanes, _ = queue_sim.run(feed, placements)
    lines = ["model,order_id,ts,side,price,shares,trigger"]
    for model in queue_sim.MODELS:
        for f in lanes[model].fills:
            lines.append(f"{model},{f['order_id']},{f['ts']},{f['side']},"
                         f"{f['price']},{f['shares']},{f['trigger']}")
    return "\n".join(lines) + "\n"


def compare(cpp, py, label):
    if cpp == py:
        rows = max(0, len(cpp.splitlines()) - 1)
        print(f"  {label}: {rows} fill rows identical")
        return True
    print(f"  {label}: MISMATCH")
    a, b = cpp.splitlines(), py.splitlines()
    shown = 0
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "<missing>"
        y = b[i] if i < len(b) else "<missing>"
        if x != y:
            print(f"    row {i}: c++={x!r}")
            print(f"            py ={y!r}")
            shown += 1
            if shown >= 5:
                print("    ... stopping after 5")
                break
    if len(a) != len(b):
        print(f"    row count: c++={len(a)} python={len(b)}")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default="build/queue_sim")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--messages", type=int, default=4000)
    a = ap.parse_args()

    binary = Path(a.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"error: {binary} not found — build first")

    MID = 100_0000
    TICK = 100
    # A spread of placements: at the touch, deep in the book, both sides, and an
    # iceberg — the queue paths differ for each.
    placements = [
        [("B", MID - TICK, 300, 200, 0)],
        [("B", MID - 4 * TICK, 500, 300, 0)],
        [("S", MID + TICK, 300, 200, 0)],
        [("S", MID + 3 * TICK, 400, 250, 100)],
        [("B", MID - 2 * TICK, 200, 150, 0), ("S", MID + 2 * TICK, 200, 150, 0)],
    ]

    ok = True
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        for seed in range(1, a.seeds + 1):
            for bias in (0.0, 0.5, 1.0):
                feed = d / f"q{seed}_{bias}.gz"
                subprocess.run(
                    [sys.executable, str(ROOT / "python" / "make_queue_feed.py"), str(feed),
                     "--seed", str(seed), "--messages", str(a.messages),
                     "--cancel-front-bias", str(bias)],
                    check=True, capture_output=True)
                print(f"seed {seed}, cancel-front-bias {bias}:")
                for i, pl in enumerate(placements):
                    cpp = run_cpp(binary, feed, pl, d / f"c{seed}_{bias}_{i}.csv")
                    py = run_python(feed, pl)
                    if not compare(cpp, py, f"placement {i}"):
                        ok = False
    print()
    if ok:
        print("OK: C++ and the Python oracle agree byte for byte on every fill")
        return 0
    print("FAIL: the implementations disagree")
    return 1


if __name__ == "__main__":
    sys.exit(main())
