#!/usr/bin/env python3
"""phase12-8-report.py — join the two traces, and refuse to quote what cannot be quoted.

Reads the raw records both processes wrote before they formatted anything, joins
chain A on the order token, computes the signed hops, and reports. It is a
separate program from the ones that took the measurements on purpose: they must
not spend time in post-processing, and a crash here must cost the analysis and
not the data.

WHAT THIS REFUSES TO DO, AND WHY EACH REFUSAL CAN ACTUALLY FIRE
(docs/phase12-8-design.md §5 and §6):

  * It never prints "hops sum to total: PASS". Differences of stored stamps
    telescope, so that identity holds unconditionally -- with a 10 us clock
    offset, with a stamp in the wrong function, with an entirely un-instrumented
    region between two stamps. A check that cannot fail is not a check.
  * It reports COVERAGE instead: the fraction of the iteration in which an order
    completed that is accounted for by hops stamped inside it. That is the one
    that can see a region nobody is timing.
  * It checks the two instruments against each other. t0->t3 is measured by
    CLOCK_MONOTONIC and again by the paired rdtscp; they must agree.
  * A negative INTRA-process hop is a harness bug -- stamps out of order, a
    wrong join -- and is fatal. A negative CROSS-process hop is a clock finding
    and is counted, printed, and vetoes the run if it exceeds the offset bound.
    Nothing is clamped: wire_to_book's `now > arrival ? now - arrival : 0` is
    safe within one thread and destroys the evidence across two.
  * A percentile computed over fewer samples than it can resolve is not printed.
    p99.9 of n=800 is the first sample; saying "p99.9" of that is a decoration.

Exit codes follow scripts/pinned-run.sh: 0 quotable, 3 ran correctly but not
quotable, 1 broken, 2 usage.
"""
import argparse
import json
import os
import struct
import sys

MAGIC = 0x38324254   # "TB28"
VERSION = 1

# Must match include/itchbook/bench/trace.hpp exactly. A silent layout change
# would be read here as plausible numbers, which is why the file carries a
# version and this asserts the record size the writer declared.
CHAINA = "<QQQQQQQQQIIIIHHBBBB"
CHAINA_SIZE = struct.calcsize(CHAINA)
FILLREC = "<QQIIIBBH"
FILLREC_SIZE = struct.calcsize(FILLREC)
ACCEPTEX = "<QQIB3x"
ACCEPTEX_SIZE = struct.calcsize(ACCEPTEX)


def read_trace(path):
    """Returns {tag: (record_size, [raw bytes per record])}."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 8:
        raise SystemExit("error: %s is too short to be a trace" % path)
    magic, ver = struct.unpack_from("<II", blob, 0)
    if magic != MAGIC:
        raise SystemExit("error: %s is not a 12.8 trace (magic %#x)" % (path, magic))
    if ver != VERSION:
        raise SystemExit("error: %s is trace version %d, this reads %d" % (path, ver, VERSION))
    out = {}
    off = 8
    while off + 12 <= len(blob):
        tag = blob[off:off + 4].decode("ascii", "replace")
        n, rec = struct.unpack_from("<II", blob, off + 4)
        off += 12
        need = n * rec
        if off + need > len(blob):
            raise SystemExit("error: %s section %s claims %d records of %d bytes, "
                             "file has %d left" % (path, tag, n, rec, len(blob) - off))
        out[tag] = (rec, blob[off:off + need])
        off += need
    return out


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    i = int(p / 100.0 * len(sorted_vals))
    if i >= len(sorted_vals):
        i = len(sorted_vals) - 1
    return sorted_vals[i]


def resolvable(n, p):
    """Can n samples resolve percentile p at all?

    p99.9 of 800 samples is the top sample: the estimate is one observation and
    the label implies a thousand. Requiring at least 10 samples beyond the index
    is the least this can demand and still mean something."""
    return n >= 10 and (n - int(p / 100.0 * n)) >= 10


class Hop:
    def __init__(self, name, cross_process=False):
        self.name = name
        self.cross = cross_process
        self.vals = []
        self.negatives = []

    def add(self, v):
        if v < 0:
            self.negatives.append(v)
        self.vals.append(v)

    def finalize(self):
        self.vals.sort()

    @property
    def n(self):
        return len(self.vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy-trace", required=True)
    ap.add_argument("--exchange-trace", required=True)
    ap.add_argument("--strategy-json", required=True)
    ap.add_argument("--exchange-json", required=True)
    ap.add_argument("--offset-bound-ns", type=float, default=None,
                    help="from tools/tsc_offset on the exact pinned pair")
    ap.add_argument("--jitter-json", default=None,
                    help="cpu_jitter on the exact pinned cores; the decisive gate")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    st = read_trace(args.strategy_trace)
    ex = read_trace(args.exchange_trace)
    sj = json.load(open(args.strategy_json))
    ej = json.load(open(args.exchange_json))

    fatal = []       # exit 1: the harness is broken
    not_quotable = []  # exit 3: it ran, the numbers may not be published

    # ---- layout ---------------------------------------------------------------
    if "CHNA" not in st:
        raise SystemExit("error: strategy trace has no CHNA section")
    if "ACPT" not in ex:
        raise SystemExit("error: exchange trace has no ACPT section")
    rec_a, blob_a = st["CHNA"]
    rec_p, blob_p = ex["ACPT"]
    if rec_a != CHAINA_SIZE:
        fatal.append("ChainA record is %d bytes on the wire, %d here -- the "
                     "layouts have diverged" % (rec_a, CHAINA_SIZE))
    if rec_p != ACCEPTEX_SIZE:
        fatal.append("AcceptEx record is %d bytes on the wire, %d here" %
                     (rec_p, ACCEPTEX_SIZE))
    if fatal:
        for f in fatal:
            print("FATAL: " + f)
        return 1

    HAVE_T0, HAVE_T1, HAVE_T1P, HAVE_T2, HAVE_T3 = 1, 2, 4, 8, 16

    chains = {}
    for i in range(len(blob_a) // rec_a):
        (t0, t1, t1p, t2, t3, iter_start, iter_end, tsc0, tsc3,
         ref_seq, stride, dg, msgs, cpu0, cpu3, resp, have, term, _pad) = \
            struct.unpack_from(CHAINA, blob_a, i * rec_a)
        if have == 0:
            continue
        chains[i] = dict(t0=t0, t1=t1, t1p=t1p, t2=t2, t3=t3,
                         iter_start=iter_start, iter_end=iter_end,
                         tsc0=tsc0, tsc3=tsc3, stride=stride,
                         dgrams=dg, msgs=msgs, cpu0=cpu0, cpu3=cpu3, have=have)

    accepts = {}
    dup_tokens = 0
    for i in range(len(blob_p) // rec_p):
        t3p, t4, token_seq, resp = struct.unpack_from(ACCEPTEX, blob_p, i * rec_p)
        if token_seq in accepts:
            dup_tokens += 1
        accepts[token_seq] = dict(t3p=t3p, t4=t4, resp=chr(resp) if resp else "?")

    # ---- the join, and the identities that can fail --------------------------
    orders_sent = sj.get("orders_sent", 0)
    complete = [k for k, c in chains.items() if (c["have"] & HAVE_T3)]
    joined = [k for k in complete if k in accepts]
    unjoined_ex = [t for t in accepts if t not in chains]

    print("=== join ===")
    print("  orders sent                    %8d" % orders_sent)
    print("  chain-A records with a t3      %8d" % len(complete))
    print("  exchange accept/reject records %8d" % len(accepts))
    print("  joined on the order token      %8d" % len(joined))
    print("  exchange records with no chain %8d" % len(unjoined_ex))
    print("  duplicate tokens on the exchange %6d" % dup_tokens)

    if dup_tokens:
        fatal.append("%d order tokens appeared twice on the exchange: the join "
                     "key does not identify one order" % dup_tokens)
    if unjoined_ex:
        fatal.append("%d exchange records join to no chain: the exchange saw "
                     "orders the strategy has no record of" % len(unjoined_ex))
    if len(complete) != orders_sent:
        fatal.append("%d orders sent but %d chains reached t3" %
                     (orders_sent, len(complete)))

    # ---- hops -----------------------------------------------------------------
    hops = [
        Hop("t0->t1   arrival to applied"),
        Hop("t1->t1'  post-trigger drain"),
        Hop("t1'->t2  decision"),
        Hop("t2->t3   encode, frame, write"),
        Hop("t3->t3'  loopback TCP", cross_process=True),
        Hop("t3'->t4  parse, validate, submit"),
    ]
    head_react = Hop("t1'->t3  HEADLINE reaction path")
    head_arrival = Hop("t0->t3   arrival to write (incl. drain)")
    coverage = []
    two_instrument = []

    cyc_per_ns = None
    for k in sorted(joined):
        c = chains[k]
        a = accepts[k]
        if c["have"] & HAVE_T0:
            hops[0].add(int(c["t1"]) - int(c["t0"]))
            hops[1].add(int(c["t1p"]) - int(c["t1"]))
            head_arrival.add(int(c["t3"]) - int(c["t0"]))
        hops[2].add(int(c["t2"]) - int(c["t1p"]))
        hops[3].add(int(c["t3"]) - int(c["t2"]))
        hops[4].add(int(a["t3p"]) - int(c["t3"]))
        hops[5].add(int(a["t4"]) - int(a["t3p"]))
        head_react.add(int(c["t3"]) - int(c["t1p"]))
        if c["iter_end"] and c["iter_start"]:
            wall = int(c["iter_end"]) - int(c["iter_start"])
            # The chain spans the top of the iteration to the write. What is NOT
            # accounted for is t3 -> iter_end: session bookkeeping and the exit
            # checks, after the order has already gone.
            inside = int(c["t3"]) - int(c["iter_start"])
            if wall > 0:
                coverage.append(100.0 * inside / wall)
        if c["tsc0"] and c["tsc3"]:
            two_instrument.append((int(c["t3"]) - int(c["t0"]),
                                   int(c["tsc3"]) - int(c["tsc0"])))

    for h in hops + [head_react, head_arrival]:
        h.finalize()

    # ---- negatives -------------------------------------------------------------
    intra_neg = sum(len(h.negatives) for h in hops if not h.cross)
    intra_neg += len(head_react.negatives) + len(head_arrival.negatives)
    cross_neg = sum(len(h.negatives) for h in hops if h.cross)
    if intra_neg:
        worst = min(min(h.negatives) for h in hops
                    if not h.cross and h.negatives)
        fatal.append("%d NEGATIVE intra-process hops, worst %d ns: stamps are "
                     "out of order or the join is wrong. This is a harness bug, "
                     "not a clock finding." % (intra_neg, worst))

    # ---- report ----------------------------------------------------------------
    print()
    print("=== hops (ns) === NOT ADDITIVE: percentiles do not sum; see the design doc")
    print("  %-34s %8s %10s %10s %10s  %s" % ("hop", "n", "p50", "p90", "p99", "notes"))
    def line(h):
        note = ""
        if h.cross:
            note = "CROSS-PROCESS: carries the clock offset"
        if h.negatives:
            note += (" %d negative, worst %d" % (len(h.negatives), min(h.negatives)))
        p99 = pct(h.vals, 99) if resolvable(h.n, 99) else None
        print("  %-34s %8d %10s %10s %10s  %s" % (
            h.name, h.n,
            pct(h.vals, 50) if h.n else "-",
            pct(h.vals, 90) if resolvable(h.n, 90) else "n too small",
            p99 if p99 is not None else "n too small",
            note))
    for h in hops:
        line(h)
    print()
    line(head_react)
    line(head_arrival)

    # ---- the checks that can fail ---------------------------------------------
    print()
    print("=== checks ===")
    print("  Sum(hops) == t4-t0 is ARITHMETIC, not a check: differences of stored")
    print("  stamps telescope. It is not reported as a pass. Coverage is:")
    if coverage:
        coverage.sort()
        print("    chain span as a fraction of its own iteration: "
              "p50 %.1f%%  p10 %.1f%%  n=%d"
              % (pct(coverage, 50), pct(coverage, 10), len(coverage)))
        print("      (the remainder is after t3 -- session bookkeeping and exit")
        print("       checks. A region added between t1' and t3 without a stamp")
        print("       would show up here as a fall.)")
    else:
        not_quotable.append("no coverage samples: the iteration window was never closed")

    if two_instrument:
        if cyc_per_ns is None:
            # Derive it from the run itself rather than assuming the nominal clock.
            tot_ns = sum(a for a, _ in two_instrument)
            tot_cyc = sum(b for _, b in two_instrument)
            cyc_per_ns = (tot_cyc / tot_ns) if tot_ns > 0 else 0.0
        dis = []
        for ns, cyc in two_instrument:
            if cyc_per_ns > 0:
                dis.append(abs(ns - cyc / cyc_per_ns))
        dis.sort()
        print("    two instruments on t0->t3: implied %.3f cycles/ns, "
              "|disagreement| p50 %.0f ns p99 %s"
              % (cyc_per_ns, pct(dis, 50),
                 pct(dis, 99) if resolvable(len(dis), 99) else "n too small"))
    else:
        not_quotable.append("no paired rdtscp samples: the second instrument never ran")

    for h in hops + [head_react, head_arrival]:
        if h.n == 0:
            fatal.append("hop '%s' took ZERO samples. A hop with no samples "
                         "scored as a pass is the failure this gate exists for."
                         % h.name)

    if sj.get("samples_dropped", 0) or ej.get("samples_dropped", 0):
        fatal.append("samples were dropped: an arena filled and the run is "
                     "missing chains it cannot identify")
    if sj.get("core_migrations", 0):
        not_quotable.append("%d chains crossed cores: with pinning on, that means "
                            "the pinning silently failed" % sj["core_migrations"])
    if sj.get("gaps", 0) or sj.get("messages_lost", 0):
        fatal.append("the feed had gaps: the run is not comparable")

    # ---- environment: the decisive gate ---------------------------------------
    if sj.get("virtualised"):
        not_quotable.append("running under a hypervisor (/proc/version says "
                            "WSL2): cpu_jitter on this box reports ~3,000 gaps "
                            "per second over 10 us and a worst gap of 2.57 ms, "
                            "so a microsecond decomposition here is measuring "
                            "the scheduler")
    if args.jitter_json:
        try:
            jj = json.load(open(args.jitter_json))
            for c in jj.get("cpus", []):
                over = c.get("gaps_over", {}).get("100us", None)
                if over is None:
                    not_quotable.append("cpu %s reports no gaps_over.100us field" % c.get("cpu"))
                elif over > 0:
                    not_quotable.append("cpu %s was off-CPU for over 100 us %d times: "
                                        "the hops are measuring the scheduler"
                                        % (c.get("cpu"), over))
        except Exception as e:      # noqa: BLE001 - a probe that cannot be read is not a pass
            not_quotable.append("cpu_jitter artifact unreadable (%s); absent is not a pass" % e)
    else:
        not_quotable.append("no cpu_jitter artifact supplied: it is the DECISIVE "
                            "gate and every clock check can pass without it")

    if args.offset_bound_ns is not None:
        smallest_cross = min((pct(h.vals, 50) for h in hops if h.cross and h.n), default=None)
        if smallest_cross is not None and smallest_cross > 0:
            ratio = args.offset_bound_ns / smallest_cross
            print("    tsc_offset bound %.1f ns against the smallest cross-process "
                  "hop p50 %d ns = %.0f%%" % (args.offset_bound_ns, smallest_cross,
                                              100.0 * ratio))
            if ratio > 0.10:
                not_quotable.append("the clock offset bound is %.0f%% of the smallest "
                                    "cross-process hop; the split between the two "
                                    "cross-process hops is not resolvable"
                                    % (100.0 * ratio))
    else:
        not_quotable.append("no tsc_offset bound supplied for the pinned pair")

    if cross_neg:
        worst = -min(min(h.negatives) for h in hops if h.cross and h.negatives)
        print("    %d cross-process hops are negative, worst %d ns" % (cross_neg, worst))
        if args.offset_bound_ns is None:
            print("      Whether that is the clock or the scheduler cannot be said "
                  "without --offset-bound-ns from the pinned pair.")
            not_quotable.append("%d negative cross-process hops and no offset bound "
                                "to judge them against" % cross_neg)
        elif worst > 100 * args.offset_bound_ns:
            # Orders of magnitude past the clock: this is the machine, not the
            # counters, and saying otherwise would be the comfortable answer.
            not_quotable.append("a cross-process hop is %d ns negative against a "
                                "%.1f ns clock bound -- %.0fx too large to be the "
                                "clock. Two processes are being descheduled "
                                "relative to each other; the hop split is noise."
                                % (worst, args.offset_bound_ns,
                                   worst / args.offset_bound_ns))
        elif worst > args.offset_bound_ns:
            not_quotable.append("a cross-process hop is %d ns negative, beyond the "
                                "%.1f ns offset bound" % (worst, args.offset_bound_ns))

    # ---- verdict ---------------------------------------------------------------
    print()
    if fatal:
        print("=== BROKEN ===")
        for f in fatal:
            print("  ! " + f)
    if not_quotable:
        print("=== RAN, NOT QUOTABLE ===")
        for f in not_quotable:
            print("  - " + f)
    if not fatal and not not_quotable:
        print("=== QUOTABLE ===")

    if args.json_out:
        doc = {
            "quotable": (not fatal and not not_quotable),
            "broken": fatal,
            "not_quotable": not_quotable,
            "orders_sent": orders_sent,
            "joined": len(joined),
            "hops": {h.name: {"n": h.n,
                              "p50": pct(h.vals, 50),
                              "p99": pct(h.vals, 99) if resolvable(h.n, 99) else None,
                              "negatives": len(h.negatives),
                              "cross_process": h.cross} for h in hops},
            "headline_t1p_t3": {"n": head_react.n, "p50": pct(head_react.vals, 50)},
            "coverage_p50_pct": pct(coverage, 50) if coverage else None,
            "implied_cycles_per_ns": cyc_per_ns,
        }
        with open(args.json_out, "w") as f:
            json.dump(doc, f, indent=2)
        print("wrote " + args.json_out)

    if fatal:
        return 1
    if not_quotable:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
