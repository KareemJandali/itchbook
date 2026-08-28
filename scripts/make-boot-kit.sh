#!/usr/bin/env bash
#
# make-boot-kit.sh — everything phase 12.8's bare-metal run needs, on one stick.
#
# The live session has no compiler, no network, and loses everything at reboot.
# Every hour spent there is expensive and unrepeatable without another reboot, so
# nothing that can be prepared beforehand is left to be discovered on the day.
#
# WHY THE BINARIES ARE STATIC. There is no toolchain in the live session and no
# guarantee the distribution's glibc matches. A dynamically linked binary that
# will not start is a boot spent on nothing. `ldd` is run against each one and
# the kit refuses to assemble if any of them still wants a shared object.
#
# WHY THE FEED IS COPIED AND NOT COMMITTED. data/sliced/MSFT.gz is licensed
# market data. It goes on the stick and it never goes in the tree -- .gitignore
# covers data/raw, data/sliced and *.gz, and this script writes outside the
# repository for the same reason.
#
# WHAT THE KIT REFUSES TO DO. It will not assemble from a dirty tree without
# saying so, because the whole point of the artifact's commit field is that a
# reader can get back to the code that produced it.
#
set -uo pipefail

KIT="${KIT:-$HOME/itchbook-bootkit}"
SRC="${SRC:-$(pwd)}"
CXX="${CXX:-g++}"

cd "$SRC" || { echo "error: cannot cd to $SRC" >&2; exit 2; }
if [[ ! -f tools/exchange.cpp ]]; then
    echo "error: run this from the repository root" >&2; exit 2
fi

COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
DIRTY=no
if ! git diff --quiet 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
    DIRTY=yes
fi

echo "=== phase 12.8 boot kit"
echo "  source  $SRC"
echo "  commit  $COMMIT  (dirty: $DIRTY)"
echo "  kit     $KIT"
if [[ "$DIRTY" == yes ]]; then
    echo
    echo "  WARNING: the tree is dirty. The run's artifact will record that, and"
    echo "           the numbers will be marked not-quotable for it. Commit first"
    echo "           unless you mean to throw the run away."
fi

rm -rf "$KIT"
mkdir -p "$KIT/bin" "$KIT/data" "$KIT/scripts" "$KIT/out"

# ---- 1. static binaries -------------------------------------------------------
echo
echo "=== building static"
BUILT=()
for t in exchange strategy tsc_offset cpu_jitter; do
    printf '  %-12s' "$t"
    if $CXX -std=c++20 -O2 -static -I include -o "$KIT/bin/$t" "tools/$t.cpp" -lz -lpthread 2>"$KIT/$t.build.log"; then
        echo "ok  $(stat -c %s "$KIT/bin/$t" | numfmt --to=iec)"
        BUILT+=("$t")
    else
        echo "FAILED"
        tail -5 "$KIT/$t.build.log"
        exit 1
    fi
done

# A binary that still wants a shared object is a boot spent on nothing.
echo
echo "=== confirming they are actually static"
for t in "${BUILT[@]}"; do
    # ldd EXITS 1 on a static binary, and `set -o pipefail` turns that into a
    # failed pipeline even when the grep matched -- which reported a correctly
    # static binary as still dynamic. Capture first, test second.
    LDD_OUT="$(ldd "$KIT/bin/$t" 2>&1 || true)"
    if printf '%s' "$LDD_OUT" | grep -q "not a dynamic executable"; then
        echo "  $t  static"
    else
        echo "  $t  STILL DYNAMIC -- it may not start in the live session:"
        printf '%s\n' "$LDD_OUT" | head -5
        exit 1
    fi
done

# ...and that they run at all here, which is the cheapest possible smoke test.
echo
echo "=== smoke test"
"$KIT/bin/tsc_offset" --samples 2000 > "$KIT/out/tsc-smoke.txt" 2>&1 \
    && echo "  tsc_offset  runs" || { echo "  tsc_offset FAILED"; exit 1; }
"$KIT/bin/cpu_jitter" --cpus 0 --seconds 1 > "$KIT/out/jitter-smoke.txt" 2>&1 \
    && echo "  cpu_jitter  runs" || { echo "  cpu_jitter FAILED"; exit 1; }
# Usage goes to STDERR, so redirect it or the test silently checks nothing --
# which is how the first version of this reported "check manually" against a
# binary that was working perfectly.
# Both of these exit NON-ZERO on the path being tested -- exchange returns 2 to
# print its usage -- and pipefail would take that as the pipeline's status
# regardless of what the grep found. Capture, then test.
SMOKE_OUT="$("$KIT/bin/exchange" 2>&1 || true)"
if printf '%s' "$SMOKE_OUT" | head -1 | grep -qi usage; then
    echo "  exchange    runs"
else
    echo "  exchange    did not print its usage; check it by hand"
    printf '%s\n' "$SMOKE_OUT" | head -3
    exit 1
fi
SMOKE_OUT="$("$KIT/bin/strategy" --nonsense 2>&1 || true)"
if printf '%s' "$SMOKE_OUT" | head -1 | grep -qi "unknown option"; then
    echo "  strategy    runs"
else
    echo "  strategy    did not reject a bad option; check it by hand"
    printf '%s\n' "$SMOKE_OUT" | head -3
    exit 1
fi

# ---- 2. the scripts the run needs ---------------------------------------------
echo
echo "=== scripts"
for f in tick-to-trade.sh phase12-8-report.py tick-to-trade-mtu-sweep.sh; do
    cp "scripts/$f" "$KIT/scripts/" && echo "  $f"
done
cp docs/phase12-8-design.md "$KIT/" && echo "  phase12-8-design.md (the checklist is section 9)"

# ---- 3. the feed, which is licensed and never committed -----------------------
echo
echo "=== feed"
if [[ -f data/sliced/MSFT.gz ]]; then
    cp data/sliced/MSFT.gz "$KIT/data/" && \
        echo "  MSFT.gz  $(stat -c %s "$KIT/data/MSFT.gz" | numfmt --to=iec)  (licensed: stick only, never the tree)"
else
    echo "  MSFT.gz MISSING. Slice it first:"
    echo "    ./build/itch_slice data/raw/<day>.NASDAQ_ITCH50.gz MSFT data/sliced/MSFT.gz"
    exit 1
fi
if [[ -f data/raw/bench.gz ]]; then
    cp data/raw/bench.gz "$KIT/data/" && echo "  bench.gz (generated; a fallback if the slice will not read)"
fi

# ---- 4. the runbook and the save script ---------------------------------------
cat > "$KIT/RUNBOOK.txt" <<'EOF'
PHASE 12.8 -- BARE METAL RUNBOOK
================================
Everything here is static. There is no compiler and no network in this session,
and everything is lost at reboot. Do step 6 before you shut down.

1. cd into this kit, wherever the stick is mounted.
       cd /media/*/itchbook-bootkit    (or wherever it landed)

2. LOOK AT THE MACHINE. Run the harness with no CPUs set. It will refuse and
   print the topology, marking which cores are isolated.

       BIN=bin OUT=out ./scripts/tick-to-trade.sh

   Pick TWO cpus that are:
     - not the same cpu
     - NOT SMT siblings (the printed "siblings=" must not contain the other)
     - the same package
     - isolated, if any are

   On the WSL2 box these were 13 and 14. DO NOT assume that holds here --
   the layout is different on different machines and nproc lies about it.

3. DRY RUN FIRST. Short, and it must show every hop taking samples.

       BIN=bin OUT=out CPU_A=<a> CPU_B=<b> \
       FEED=data/MSFT.gz SYMBOL=MSFT LOCATE=5291 \
       REPEATS=1 DRY_LIMIT=120000 LIMIT=400000 TAG=dry \
       ./scripts/tick-to-trade.sh

   If it stops at cpu_jitter, the machine is not quiet enough and the numbers
   would be measuring the scheduler. That is the gate doing its job; try
   isolated cores, or accept exit 3 and know the run is not quotable.

4. THE REAL RUN. Ten repeats, full day, about 62 s each.

       BIN=bin OUT=out CPU_A=<a> CPU_B=<b> \
       FEED=data/MSFT.gz SYMBOL=MSFT LOCATE=5291 \
       REPEATS=10 MULTIPLIER=1000 TAG=baremetal \
       ./scripts/tick-to-trade.sh

   Exit 0 means quotable. Exit 3 means it ran correctly and the numbers may
   not be published -- the artifact is written either way and says why.

5. THE SWEEPS, if step 4 came back 0. Each is one run.
       - MULTIPLIER=500 and MULTIPLIER=2000, TAG=mult500 / mult2000
         (the resting interval must scale as 1/multiplier; the reaction path
          must not move. That is what proves which hops are the machine.)
       - the MTU sweep, which halves the packing delay:
             BIN=bin ./scripts/tick-to-trade-mtu-sweep.sh
       - one run with --ack-delay-ms 250 passed through, as the t6 placement
         test: fills must still be counted and the shares must still agree.

6. SAVE EVERYTHING BEFORE YOU REBOOT. This session forgets.

       ./save-to-stick.sh /media/<you>/<stick>

   Then check the files are really there before shutting down.

DO NOT use --rt-priority or SCHED_FIFO. Phase 10 established that RT priority
was itself the source of a 47 ms lateness spike via RT bandwidth throttling, and
dropping it took the sender p99 from 26,603,394 ns to 116,005 ns.
EOF
echo
echo "=== runbook"
echo "  RUNBOOK.txt"

cat > "$KIT/save-to-stick.sh" <<'EOF'
#!/usr/bin/env bash
# Copy every artifact off before the live session forgets it.
set -uo pipefail
DEST="${1:-}"
if [[ -z "$DEST" ]]; then echo "usage: $0 /path/to/stick" >&2; exit 2; fi
STAMP="$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo run)"
OUTDIR="$DEST/itchbook-12-8-$STAMP"
mkdir -p "$OUTDIR" || { echo "cannot write $OUTDIR" >&2; exit 1; }
cp -r out "$OUTDIR/" 2>/dev/null
# The traces are large; take the JSON and the reports for certain, traces if they fit.
find /tmp -maxdepth 2 -name '*-report.json' -newer RUNBOOK.txt -exec cp {} "$OUTDIR/" \; 2>/dev/null
cp -r /tmp/tmp.*/ "$OUTDIR/work/" 2>/dev/null
sync
echo "saved to $OUTDIR:"
ls -la "$OUTDIR"
echo
echo "CHECK THE FILES ARE REALLY THERE BEFORE YOU REBOOT."
EOF
chmod 755 "$KIT/save-to-stick.sh"
echo "  save-to-stick.sh"

# ---- 5. provenance, inside the kit --------------------------------------------
cat > "$KIT/PROVENANCE.txt" <<EOF
built from : $COMMIT
dirty tree : $DIRTY
built on   : $(uname -sr)
compiler   : $($CXX --version | head -1)
binaries   : $(cd "$KIT/bin" && sha256sum * | sed 's/^/             /')
EOF
echo "  PROVENANCE.txt"

echo
echo "=== kit ready: $KIT"
du -sh "$KIT"
echo
echo "Copy the whole directory to the stick, then follow RUNBOOK.txt on the machine."
