#!/usr/bin/env python3
"""Re-run the exact mutations the audit proved went unnoticed.

Every one of these was applied by an adversarial audit against the FIRST version
of these tests, rebuilt, re-run, and reported "all checks passed". If the
repaired tests are worth anything, every one is now caught. A mutation that
still passes is a test that is still decoration, and is reported as MISSED.

Rules carried over from the 12.7 harness, both learned expensively: a green
baseline is required first, and a mutant that fails to COMPILE is an ERROR
rather than a catch -- a detector that cannot run has not detected anything.
"""
import io, os, shutil, subprocess, sys

ROOT = "/home/karee/itchbook"
MUTATIONS = [
    ("arena aliases slot 0 instead of advancing",
     "include/itchbook/bench/trace.hpp",
     "        T* p = &store_[used_++];",
     "        T* p = &store_[0]; ++used_;   // MUTANT",
     "cpp"),

    ("every probe reports ABSENT forever",
     "include/itchbook/bench/topology.hpp",
     "    Probe p;\n    std::FILE* f = std::fopen(path.c_str(), \"r\");",
     "    Probe p;\n    return p;   // MUTANT\n    std::FILE* f = std::fopen(path.c_str(), \"r\");",
     "cpp"),

    ("thread_cpu_ns returns a constant zero",
     "include/itchbook/bench/rdtsc.hpp",
     "inline uint64_t thread_cpu_ns() {\n    timespec ts;",
     "inline uint64_t thread_cpu_ns() {\n    return 0;   // MUTANT\n    timespec ts;",
     "cpp"),

    ("thread CPU clock replaced by the wall clock",
     "include/itchbook/bench/rdtsc.hpp",
     "    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);",
     "    clock_gettime(CLOCK_MONOTONIC, &ts);   // MUTANT",
     "cpp"),

    ("ChainA tsc0 and tsc3 swapped",
     "include/itchbook/bench/trace.hpp",
     "    uint64_t tsc0 = 0;      // paired rdtscp at t0, second instrument\n"
     "    uint64_t tsc3 = 0;      // paired rdtscp at t3",
     "    uint64_t tsc3 = 0;   // MUTANT: swapped\n    uint64_t tsc0 = 0;",
     "cpp"),

    ("FillRec ref_seq and fill_ordinal swapped (chain B's join key inverted)",
     "include/itchbook/bench/trace.hpp",
     "    uint32_t ref_seq = 0;\n    uint32_t fill_ordinal = 0;   // n-th fill of THIS reference; see below",
     "    uint32_t fill_ordinal = 0;   // MUTANT: swapped\n    uint32_t ref_seq = 0;",
     "cpp"),

    ("the census ignores the CPU clock and thresholds wall time",
     "scripts/phase12-8-report.py",
     '        off = (int(c["t3"]) - int(c["t1p"])) - (int(c["cpu_t3"]) - int(c["cpu_t1p"]))',
     '        off = int(c["t3"]) - int(c["t1p"])   # MUTANT',
     "py"),

    ("the census gate is deleted, so p99.9 prints unattributed",
     "scripts/phase12-8-report.py",
     '        not_quotable.append("no gap-overlap census: p99.9 cannot be attributed")',
     '        pass   # MUTANT',
     "py"),

    ("the report joins on the exchange reference instead of the order token",
     "scripts/phase12-8-report.py",
     "        chains[i] = dict(t0=t0,",
     "        chains[ref_seq] = dict(t0=t0,   # MUTANT",
     "py"),

    ("the duplicate-token fatal is removed",
     "scripts/phase12-8-report.py",
     '        fatal.append("%d order tokens appeared twice on the exchange: the join "',
     '        _unused = ("%d order tokens appeared twice on the exchange: the join "',
     "py"),

    ("the negative intra-process hop fatal is removed",
     "scripts/phase12-8-report.py",
     '        fatal.append("%d NEGATIVE intra-process hops, worst %d ns: stamps are "',
     '        _unused2 = ("%d NEGATIVE intra-process hops, worst %d ns: stamps are "',
     "py"),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, shell=True, capture_output=True, text=True)


def tests_pass():
    b = run("cmake --build build --target test_tick_to_trade")
    if b.returncode != 0:
        return None, "does not compile:\n" + (b.stdout + b.stderr)[-500:]
    c = run("./build/test_tick_to_trade")
    p = run("python3 tests/test_report_join.py")
    ok = (c.returncode == 0 and p.returncode == 0)
    detail = ""
    if not ok:
        lines = [l for l in (c.stdout + c.stderr + p.stdout + p.stderr).splitlines()
                 if "FAIL" in l or "not a per-thread" in l]
        detail = "; ".join(l.strip() for l in lines[:3])
    return ok, detail


print("=== baseline: both test files must PASS before any mutation means anything")
ok, detail = tests_pass()
if ok is None or not ok:
    print("ERROR: baseline is not green -- nothing below would be meaningful")
    print(detail)
    sys.exit(2)
print("    baseline green\n")

caught = missed = errors = 0
for name, path, old, new, kind in MUTATIONS:
    full = os.path.join(ROOT, path)
    src = io.open(full, encoding="utf-8").read()
    n = src.count(old)
    print("=== %s" % name)
    if n != 1:
        print("    ERROR: anchor matched %d times in %s -- the mutation was never")
        print("           applied, so this is not a catch." % ())
        errors += 1
        continue
    backup = src
    io.open(full, "w", encoding="utf-8").write(src.replace(old, new, 1))
    try:
        ok, detail = tests_pass()
    finally:
        io.open(full, "w", encoding="utf-8").write(backup)
    if ok is None:
        print("    ERROR: mutant does not compile. A detector that cannot run has")
        print("           not detected anything.")
        errors += 1
    elif ok:
        print("    *** MISSED: the tests passed with this defect in the tree ***")
        missed += 1
    else:
        print("    CAUGHT: %s" % (detail or "tests failed"))
        caught += 1

# The tree must be exactly as it was.
run("cmake --build build --target test_tick_to_trade")
print("\n=== %d caught, %d missed, %d errors, of %d mutations ==="
      % (caught, missed, errors, len(MUTATIONS)))
sys.exit(0 if (missed == 0 and errors == 0) else 1)
