#!/usr/bin/env python3
"""CI gate for the phase 4 optimisation: assert the pool change still pays.

    python3 bench/regression_check.py --messages 400000

The build plan's testing table lists a Performance row whose stated purpose is
"Regressions". Measurement existed — bench/baseline.json and bench/after.json —
but nothing compared against them, so the row was a measurement, not a gate.

An ABSOLUTE gate on those files cannot work: they were recorded on one machine
and CI runs on a shared runner whose speed varies by more than the effect being
guarded. So this gates on a RATIO between two binaries built from the same
source on the same machine in the same run, differing only in the pool's chunk
policy. Machine speed cancels out, exactly as it does in
python/analysis/scaling_check.py.

What it is actually protecting. Phase 4's win was not in the hot path: it was
one 42MB slab being faulted in on first touch, which showed up as a single
message costing tens of millions of cycles. Geometric chunks spread that cost
across the run. So the two things that must stay true are that the worst
message got dramatically cheaper, and that throughput did not regress.

The thresholds are deliberately loose. This is a tripwire for someone
reintroducing a big up-front allocation, not a benchmark: the real numbers are
1.6-1.7x on cycles and 80-100x on the worst message, and the gate fires at 1.2x
and 10x. A gate set at the measured value fails on noise and gets disabled,
which is worse than no gate.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The "before" pool: one fixed 1M-order chunk, which is the 42MB slab.
OLD_POOL = ['-DITCHBOOK_POOL_FIRST_CHUNK=(1u<<20)', '-DITCHBOOK_POOL_MAX_CHUNK=(1u<<20)']

MIN_CYCLES_RATIO = 1.2      # measured 1.6-1.7x
MIN_MAX_RATIO = 10.0        # measured 80-100x


def build(cxx, out, extra):
    cmd = [cxx, '-std=c++20', '-O3', f'-I{ROOT}/include', *extra,
           str(ROOT / 'tools' / 'book_bench.cpp'), '-lz', '-o', out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-2000:], file=sys.stderr)
        raise SystemExit(f'error: failed to build {out}')


def run(binary, feed, tmp):
    js = os.path.join(tmp, os.path.basename(binary) + '.json')
    r = subprocess.run([binary, feed, '--json', js], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:], file=sys.stderr)
        raise SystemExit(f'error: {binary} failed')
    return json.loads(Path(js).read_text())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--messages', type=int, default=400000,
                    help='benchmark feed size (default 400000)')
    ap.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    a = ap.parse_args()

    if shutil.which(a.cxx) is None:
        raise SystemExit(f'error: no such compiler: {a.cxx}')

    with tempfile.TemporaryDirectory() as tmp:
        feed = os.path.join(tmp, 'bench.gz')
        subprocess.run([sys.executable, str(ROOT / 'python' / 'make_bench_feed.py'),
                        feed, '--messages', str(a.messages)],
                       check=True, capture_output=True)

        before_bin = os.path.join(tmp, 'bench_before')
        after_bin = os.path.join(tmp, 'bench_after')
        build(a.cxx, before_bin, OLD_POOL)
        build(a.cxx, after_bin, [])

        before = run(before_bin, feed, tmp)
        after = run(after_bin, feed, tmp)

    cyc_ratio = before['cycles_per_msg'] / after['cycles_per_msg']
    max_ratio = before['overall']['max'] / max(after['overall']['max'], 1)

    print(f'{"":22} {"one 42MB slab":>16} {"geometric chunks":>18} {"ratio":>8}')
    print('-' * 68)
    print(f'{"cycles/msg":22} {before["cycles_per_msg"]:>16.2f} '
          f'{after["cycles_per_msg"]:>18.2f} {cyc_ratio:>7.2f}x')
    print(f'{"worst message":22} {before["overall"]["max"]:>16,} '
          f'{after["overall"]["max"]:>18,} {max_ratio:>7.1f}x')
    print(f'{"p99":22} {before["overall"]["p99"]:>16,} {after["overall"]["p99"]:>18,}')
    print('-' * 68)

    bad = []
    if cyc_ratio < MIN_CYCLES_RATIO:
        bad.append(f'cycles/msg ratio {cyc_ratio:.2f}x is below the {MIN_CYCLES_RATIO}x floor')
    if max_ratio < MIN_MAX_RATIO:
        bad.append(f'worst-message ratio {max_ratio:.1f}x is below the {MIN_MAX_RATIO}x floor')

    if bad:
        for b in bad:
            print(f'FAIL: {b}', file=sys.stderr)
        print('\nThe phase 4 optimisation no longer pays. Either the pool\'s chunk\n'
              'policy regressed, or something else now allocates up front and\n'
              'dominates what this was measuring.', file=sys.stderr)
        return 1

    print('OK: the pool change still pays on this machine.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
