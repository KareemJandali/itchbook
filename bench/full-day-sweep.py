#!/usr/bin/env python3
"""Sweep one knob across a whole-day replay, interleaved and honestly.

    python3 bench/full-day-sweep.py ~/Desktop/12302019.NASDAQ_ITCH50.gz \
        --sweep band --rounds 3 --out validation/sweep-band.json

    python3 bench/full-day-sweep.py <feed> --sweep pool --rounds 3
    python3 bench/full-day-sweep.py <feed> --sweep load --rounds 3

Three knobs, three predictions written into docs/build-plan-9-12.md section 9.9
BEFORE any of this ran:

  band  dense band slots per side. Not a prediction so much as an open question:
        9.8 graded one width and did not sweep it.
  pool  one shared order pool against one per symbol. Predicted: shared wins,
        because allocation order follows arrival order, so orders that arrive
        together sit together.
  load  reference map slots against a 1.92M peak. Predicted FLAT between 25%
        and 50% load, because the probe is one cache line either way and the
        table is far past L2 in both.

The rules are phase 4's, and they are not optional:

  * INTERLEAVE. A whole-day replay takes a minute and a half; machine state
    drifts over the ten that a sweep takes. Running all of A then all of B
    attributes that drift to the change under test.
  * MEDIANS, not means. One descheduled run has no business moving the answer.
  * PIN, where the platform has taskset. macOS does not, and this says so
    rather than pretending the numbers are as tight as a pinned Linux run's.
  * REFUSE small deltas. Anything inside the observed spread is not a result.

And one thing compare.py does not have to do: every variant must reconstruct
the SAME BOOK. A knob that changes what the day looked like is not a knob, it
is a bug, and the sweep checks that before it reports a single timing.
"""
import argparse
import json
import shutil
import statistics as st
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Columns that describe the band rather than the day. Everything else in the
# per-symbol file has to be identical across every variant in a sweep.
BAND_COLUMNS = {"overflow_levels", "off_band_adds", "recentres"}

SWEEPS = {
    "band": ("band slots per side",
             [("512", ["--band-levels", "512"]),
              ("128", ["--band-levels", "128"]),
              ("2048", ["--band-levels", "2048"]),
              ("8192", ["--band-levels", "8192"])]),
    "pool": ("pool arrangement",
             [("shared", []),
              ("per-book/64", ["--per-book-pools", "--pool-first-chunk", "64"]),
              ("per-book/256", ["--per-book-pools", "--pool-first-chunk", "256"])]),
    "load": ("reference map slots (peak live orders was 1,924,078)",
             [("4M slots (46% load)", ["--refs-capacity", "4194304"]),
              ("8M slots (23% load)", ["--refs-capacity", "8388608"]),
              ("2M slots (92% load)", ["--refs-capacity", "2097152"])]),
}


def run_once(binary, feed, extra, out_json, out_csv, cpu):
    cmd = []
    if cpu is not None and shutil.which("taskset"):
        cmd += ["taskset", "-c", str(cpu)]
    cmd += [str(binary), str(feed), "--all-symbols", "--quiet",
            "--json", str(out_json), "--per-symbol", str(out_csv), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"error: {' '.join(cmd)}\n{r.stderr}")
    return json.loads(Path(out_json).read_text())


def core_rows(path):
    import csv
    out = []
    for row in csv.DictReader(Path(path).open()):
        out.append({k: v for k, v in row.items() if k not in BAND_COLUMNS})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed")
    ap.add_argument("--sweep", choices=sorted(SWEEPS), required=True)
    ap.add_argument("--binary", default="build-release/book_replay")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--cpu", type=int, default=2)
    ap.add_argument("--out")
    args = ap.parse_args()

    binary = (ROOT / args.binary).resolve()
    if not binary.exists():
        sys.exit(f"error: no such binary: {binary}. Build Release first.")
    label, variants = SWEEPS[args.sweep]

    pinned = shutil.which("taskset") is not None
    print(f"sweeping {label}")
    print(f"{args.rounds} rounds, interleaved, "
          f"{'pinned to cpu ' + str(args.cpu) if pinned else 'UNPINNED (no taskset on this platform)'}\n")

    samples = {name: [] for name, _ in variants}
    facts = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        baseline_rows = None
        for r in range(args.rounds):
            for name, extra in variants:
                res = run_once(binary, args.feed, extra, tmp / "r.json", tmp / "r.csv", args.cpu)
                samples[name].append(res)
                facts.setdefault(name, res)
                rows = core_rows(tmp / "r.csv")
                if baseline_rows is None:
                    baseline_rows = rows
                elif rows != baseline_rows:
                    sys.exit(f"error: variant '{name}' reconstructed a different day. "
                             "A knob that changes the book is not a knob.")
                print(f"  round {r + 1}  {name:<22} {res['elapsed_seconds']:7.2f} s  "
                      f"{res['peak_rss_bytes'] / 1e6:7.1f} MB")

    print(f"\n{'variant':<22} {'seconds':>9} {'spread':>8} {'M msg/s':>9} "
          f"{'peak MB':>9} {'vs first':>9}")
    print("-" * 70)
    rows_out = []
    base = None
    for name, _ in variants:
        secs = [s["elapsed_seconds"] for s in samples[name]]
        med = st.median(secs)
        spread = (max(secs) - min(secs)) / med * 100 if med else 0.0
        peak = st.median(s["peak_rss_bytes"] for s in samples[name]) / 1e6
        if base is None:
            base = med
        rate = facts[name]["messages_read"] / med / 1e6
        print(f"{name:<22} {med:9.2f} {spread:7.1f}% {rate:9.2f} {peak:9.1f} "
              f"{med / base:8.2f}x")
        rows_out.append({"variant": name, "median_seconds": med, "spread_percent": spread,
                         "peak_rss_bytes": facts[name]["peak_rss_bytes"],
                         "pool_capacity_orders": facts[name].get("pool_capacity_orders"),
                         "pools": facts[name].get("pools"),
                         "ref_map_slots": facts[name].get("ref_map_slots"),
                         "band_levels": facts[name].get("band_levels"),
                         "off_band_adds": facts[name].get("off_band_adds"),
                         "samples": secs})

    worst = max(r["spread_percent"] for r in rows_out)
    best, slowest = min(r["median_seconds"] for r in rows_out), max(r["median_seconds"] for r in rows_out)
    delta = (slowest - best) / best * 100
    print()
    if delta < worst:
        print(f"the spread between variants ({delta:.1f}%) is inside the spread WITHIN one "
              f"({worst:.1f}%).")
        print("On this machine that is not a result. Report it as flat.")
    else:
        print(f"largest difference {delta:.1f}%, against a within-variant spread of {worst:.1f}%.")
    print("Every variant reconstructed a byte-identical book.")

    if args.out:
        Path(args.out).write_text(json.dumps(
            {"sweep": args.sweep, "label": label, "rounds": args.rounds,
             "pinned": pinned, "feed": args.feed, "variants": rows_out}, indent=2) + "\n")
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
