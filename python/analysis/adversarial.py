#!/usr/bin/env python3
"""The phase 7 done-condition, as a scenario matrix with verdicts.

    python3 python/analysis/adversarial.py data/sliced/MSFT.gz --build build

The plan states it: "Take a real trading day and inject: dropped packets,
reordered packets, duplicated packets, a mid-day disconnect at 14:00, a trading
halt. The system must either produce correct state or halt safely. Never
silently wrong."

That is a tri-state, and stating it precisely is most of the work. "Correct or
safe" is not the same as "correct or incorrect", and a harness that only checks
whether the book matches the truth cannot tell the difference between a system
that survived and one that got lucky.

    CORRECT     the book matches the undamaged replay, and the system said it
                was trusted. It survived and it knew.

    SAFE        the book does NOT match, and the system said so — recovering,
                or halted. It was damaged and it did not pretend otherwise.
                This is a pass. Losing 800 messages and reconstructing them
                from nothing is not on offer; knowing they are missing is.

    CAUTIOUS    the book matches, but the system still called itself untrusted.
                Not a failure — it is the safe direction — but counted,
                because a system that always says "untrusted" is trivially
                never wrong and also useless. If this column fills up, the
                convergence bar is set too high to be worth anything.

    WRONG       the book does not match and the system called itself trusted.
                Silently wrong. This is the failure the whole phase is named
                after, and any occurrence fails the run.

A fifth outcome is possible and is also a failure: a scenario that did no
damage at all. The first version of this harness reported CORRECT for both
disconnect cases, which looked like a robust system and was in fact a
mis-specified test — the feed spanned two minutes from 09:30, so the 14:00
outage never fired. A test that does not fire is indistinguishable from a
test that passes, so every damaging scenario now has to prove it damaged
something before its verdict is allowed to count.

The comparison is the whole book — every level, both sides, price and shares
and order count — not top-of-book and not a summary total. Two books that
differ only deep in the tail are two different books, and a harness comparing
totals would report a pass while the thing under test was broken in exactly the
place a maker rests.

It is sampled MID-SESSION, not at the end, and real data is what settled that.
A real trading day ends with every order cancelled, so the end-of-day book is
empty — and comparing two empty books compares nothing. Pointed at MSFT the
first version printed `truth: 0 levels` and then graded ten scenarios against
it, every one of them trivially matching. The verdicts were unearned and the
matrix was decorative. The checkpoint is taken well after the outage and well
before the close, and a run whose checkpoint book is empty fails outright
rather than passing vacuously.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# 14:00:00 ET, in nanoseconds since midnight. The plan names this time; it is a
# mid-afternoon moment with a full book behind it and hours of feed still to
# come, which is what makes it a harder test than a disconnect near the close.
FOURTEEN_HUNDRED_NS = 14 * 3600 * 1_000_000_000


# Every entry names the damage it must actually inflict, so a scenario that
# quietly does nothing is caught rather than counted as a pass. The key is a
# field of mold_damage's report.
SCENARIOS = [
    # (name, damage flags, what it is meant to exercise)
    ("clean", [], None,
     "control: no damage, so anything but CORRECT is a harness bug"),
    ("drop-1-in-1000", ["--drop", "1000"], "dropped",
     "occasional loss, the common case on a busy link"),
    ("drop-1-in-100", ["--drop", "100"], "dropped",
     "heavy loss: many gaps, little time to converge between them"),
    ("duplicate-1-in-100", ["--duplicate", "100"], "duplicated",
     "retransmissions; applying one twice double-counts its executions"),
    ("reorder-1-in-100", ["--reorder", "100"], "reordered",
     "arrives early, looks exactly like loss, must NOT be treated as it"),
    ("truncate-1-in-500", ["--truncate", "500"], "truncated",
     "a datagram clipped mid-block; the count field cannot be trusted"),
    ("disconnect-short", ["--disconnect-for", "40"], "disconnected",
     "the plan's mid-day outage: one hole, thousands of messages"),
    ("disconnect-long", ["--disconnect-for", "400"], "disconnected",
     "a long outage, near the limit of what converges before the close"),
    ("everything", ["--drop", "500", "--duplicate", "500", "--reorder", "500",
                    "--truncate", "2000", "--disconnect-for", "40"], "dropped",
     "all of it at once, because failures compose and are rarely alone"),
    ("disconnect-to-end", ["--disconnect-for", "100000"], "disconnected",
     "the link dies and never comes back: no later packet reveals the jump, "
     "so nothing announces the loss"),
    ("halt", ["--halt-for-ns", "60000000000"], "halted",
     "the plan's fifth injection: a trading halt and its resume, INSERTED into "
     "the stream, so every sequence number after them shifts"),
    ("halt-and-drop", ["--halt-for-ns", "60000000000", "--drop", "500"], "halted",
     "a gap that straddles a halt: recovery has to survive the state change "
     "as well as the loss"),
]


def scenarios(seed, outage_ns, halt_ns):
    """The matrix, with the seed and the outage moment threaded through.

    The outage time is computed from the feed rather than hardcoded. The plan
    says 14:00, and on a real trading day that is what this resolves to; on a
    two-minute synthetic feed 14:00 is simply not in the data, and pinning it
    there produced two scenarios that silently did nothing.
    """
    out = []
    for name, flags, must, why in SCENARIOS:
        f = list(flags)
        if "--disconnect-for" in f:
            f = ["--disconnect-at-ns", str(outage_ns)] + f
        if "--halt-for-ns" in f:
            f = ["--halt-at-ns", str(halt_ns)] + f
        out.append((name, f + ["--seed", str(seed)], must, why))
    return out


def parse_damage(text):
    """mold_damage's report, as a dict."""
    d = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1].isdigit():
            d[parts[0]] = int(parts[1])
    return d


def pick_outage(first_ts, last_ts):
    """14:00 if the feed covers it, else 60% of the way through.

    Returns (ns, label). The label is printed, because a harness that
    substitutes a different time and does not say so is lying about which
    scenario it ran.
    """
    if first_ts <= FOURTEEN_HUNDRED_NS <= last_ts:
        return FOURTEEN_HUNDRED_NS, "14:00 ET"
    ns = first_ts + (last_ts - first_ts) * 6 // 10
    secs = ns / 1e9
    return ns, (f"{int(secs // 3600):02d}:{int(secs % 3600 // 60):02d}:"
                f"{secs % 60:06.3f} (feed does not reach 14:00)")


def run(cmd, allow=(0,)):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode not in allow:
        raise SystemExit(f"command failed ({r.returncode}): {' '.join(map(str, cmd))}\n"
                         f"{r.stderr.strip()}")
    return r


# ---- the live pipeline, where the damage is self-inflicted -----------------
#
# Every scenario above damages a FILE and replays it. This one damages nothing:
# the feed is perfect and the packets all arrive. The loss comes from the
# receiver, which drops whole packets when the ring is full, because the book
# thread has been told to take longer per message than the wire allows.
#
# That is the phase 10.5 promise under test -- "backpressure degrades to graded
# feed gaps rather than silent loss" -- and it is a promise about a code path
# no file-based scenario reaches. A dropped MoldUDP64 packet is exactly a
# sequence gap, so the same sequencer, the same gap policy and the same
# rebuild-forward machinery should handle it, and the same four verdicts should
# apply. If they do not, the sentence was a hope.
#
# The comparison artifact differs and the table says so: these compare the
# ALL-SYMBOLS book (one row per security, the phase 10.6 gate's artifact)
# rather than the single-symbol depth dump the file scenarios use, because the
# pipeline builds every symbol at once.
# The throttle is a MULTIPLE of the wire's own inter-message interval, not a
# fixed number of nanoseconds.
#
# It was absolute at first, and that made the scenario silently do nothing at
# any rate but the one it was tuned on: 20 us per message against a wire
# delivering one every 50 us leaves the consumer twice as fast as the producer,
# so the ring never fills and "consumer-slow" drops nothing while reporting
# CORRECT. A factor of 2 means the consumer runs at half the wire's rate on any
# machine and at any rate, which is what the scenario actually needs.
PIPELINE = [
    ("pipeline-clean", 0.0, False,
     "control: the pipeline with no throttle. Anything but CORRECT is a bug in "
     "the harness or the pipeline, not a finding"),
    ("consumer-slow", 2.0, True,
     "the book thread is throttled to half the wire's rate; the ring fills and "
     "the receiver drops whole packets"),
    ("consumer-stalled", 6.0, True,
     "a sixth of the wire's rate: sustained backpressure, and the kill switch's "
     "ring-occupancy limit should trip"),
]


def wait_ready(proc, timeout=30.0):
    """Block until the receiver says its BOOK THREAD exists.

    Waiting for the port to appear in /proc/net/udp -- which is what this used
    to do -- returns at bind(), which is 50 ms of clock calibration and an
    8.4M-entry allocation short of a consumer: 91-102 ms early, measured. The
    receiver stamps arrivals through all of it with nothing draining the ring,
    so the scenarios below would attribute that window to the pipeline they are
    grading. wire_to_book prints READY once the book thread exists.
    """
    import select
    import time
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            proc.kill()
            return False
        if not select.select([proc.stdout], [], [], remaining)[0]:
            continue
        line = proc.stdout.readline()
        if line == "":
            return False
        if line.startswith("READY"):
            return True


def pipeline_rows(b, feed, packets, work, args):
    """Run the live scenarios and return rows in the same shape as the rest."""
    import subprocess as sp

    truth_csv = work / "pipeline-truth.csv"
    run([str(b / "book_replay"), str(feed), "--all-symbols",
         "--per-symbol", str(truth_csv), "--quiet"])
    truth = truth_csv.read_text()
    if truth.count("\n") < 2:
        raise SystemExit("the all-symbols truth has no rows; nothing to compare")

    rows = []
    problems = []
    port = args.pipeline_port
    interval_ns = 1e9 / args.pipeline_rate if args.pipeline_rate > 0 else 0.0
    for name, factor, must_drop, why in PIPELINE:
        throttle_ns = int(factor * interval_ns)
        port += 1
        out_csv = work / f"{name}.csv"
        out_json = work / f"{name}.json"
        cmd = [str(b / "wire_to_book"), "--port", str(port),
               "--ring-log2", str(args.pipeline_ring_log2),
               "--timeout-ms", "6000", "--rcvbuf-mb", "16",
               "--expect-messages", "4000000",
               "--per-symbol", str(out_csv), "--json", str(out_json),
               "--recover-after", str(args.recover_after),
               "--kill-ring-occupancy", str(args.pipeline_kill_occupancy),
               "--kill-backlog-ms", "50", "--quiet"]
        if throttle_ns:
            cmd += ["--slow-consumer-ns", str(throttle_ns)]
        recv = sp.Popen(cmd, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
        if not wait_ready(recv):
            recv.kill()
            recv.wait()
            raise SystemExit(f"{name}: the receiver never reported READY on port {port}")
        run([str(b / "mold_replay_udp"), str(packets), "--host", "127.0.0.1",
             "--port", str(port), "--rate", str(args.pipeline_rate), "--quiet"])
        tail = recv.communicate()[0]
        rc = recv.returncode
        # 0 clean, 3 lossy, 4 kernel drops unreadable. Anything else is the
        # pipeline reporting a broken invariant, and that is not a verdict --
        # it is a failure of the thing doing the grading.
        if rc not in (0, 3, 4):
            raise SystemExit(f"{name}: wire_to_book exited {rc}\n{tail}")
        st = json.loads(out_json.read_text())

        # A scenario that inflicted no backpressure is not a passing scenario,
        # it is a broken one -- the same rule the file scenarios follow.
        dropped = st["ring_full_drops"]
        if must_drop and dropped == 0:
            problems.append(f"{name} (the ring never filled, so nothing was dropped)")
        if not must_drop and dropped != 0:
            problems.append(f"{name} (the control dropped {dropped} packets; "
                            f"lower --pipeline-rate or raise the ring)")
        # ...and a loss the book was never told about is the silent wrongness
        # this whole scenario exists to rule out. Since 10.10 a marker waits for
        # a slot rather than being discarded, so this counts MESSAGES that went
        # missing with no marker in front of them, and it is only reachable if
        # the consumer stopped draining altogether.
        if st["gaps_lost_to_full_ring"] != 0:
            problems.append(f"{name} ({st['gaps_lost_to_full_ring']} missing messages "
                            f"were never announced to the book)")

        rows.append({
            "name": name, "why": why,
            "verdict": verdict(out_csv.read_text() == truth, st["trusted"]),
            "lost": st["messages_lost"], "gaps": st["gaps_to_book"],
            "dups": 0, "reordered": 0, "trunc": 0,
            "state": st["state"], "rebuilds": st["rebuilds"],
            "recoveries": st["recoveries"],
            "dropped": dropped, "kill": st["kill_switch"],
            "peak": st["kill_peak_occupancy"],
        })
    return rows, problems


def verdict(matches, trusted):
    if matches and trusted:
        return "CORRECT"
    if matches and not trusted:
        return "CAUTIOUS"
    if not matches and not trusted:
        return "SAFE"
    return "WRONG"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed", help="a plain ITCH feed; it is wrapped into packets here")
    ap.add_argument("--build", default="build", help="directory holding the tools")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mtu", type=int, default=1400)
    ap.add_argument("--recover-after", type=int, default=20000,
                    help="references in the convergence window before the book "
                         "is trusted again")
    ap.add_argument("--checkpoint-frac", type=float, default=0.9,
                    help="where in the feed's time span to compare books "
                         "(default 0.9 — after the outage, before the close)")
    ap.add_argument("--pipeline", action="store_true",
                    help="also grade the LIVE pipeline: wire_to_book with its "
                         "book thread throttled until the ring fills and the "
                         "receiver drops packets (phase 10.5's promise that "
                         "backpressure becomes a graded gap)")
    ap.add_argument("--pipeline-rate", type=int, default=60000)
    ap.add_argument("--pipeline-ring-log2", type=int, default=12,
                    help="a SMALL ring on purpose: the throttled scenarios have "
                         "to fill it inside one run, and a large one turns them "
                         "into no-ops that report CORRECT")
    ap.add_argument("--pipeline-port", type=int, default=27400)
    ap.add_argument("--pipeline-kill-occupancy", type=int, default=2000)
    ap.add_argument("--keep", metavar="DIR",
                    help="keep the damaged feeds and books here instead of a "
                         "temporary directory")
    a = ap.parse_args()

    b = Path(a.build)
    needed = ["mold_wrap", "mold_damage", "mold_replay"]
    if a.pipeline:
        needed += ["wire_to_book", "mold_replay_udp", "book_replay"]
    for tool in needed:
        if not (b / tool).exists():
            raise SystemExit(f"error: {b / tool} not found — build first")

    work = Path(a.keep) if a.keep else None
    tmp = None
    if work is None:
        tmp = tempfile.TemporaryDirectory()
        work = Path(tmp.name)
    work.mkdir(parents=True, exist_ok=True)

    packets = work / "feed.mold.gz"
    print(f"wrapping {a.feed} into MoldUDP64 packets (mtu {a.mtu})")
    run([str(b / "mold_wrap"), a.feed, str(packets), "--mtu", str(a.mtu)])

    # The truth: the same feed with nothing done to it.
    truth_book = work / "truth.csv"
    run([str(b / "mold_replay"), str(packets), "--book-out", str(truth_book),
         "--quiet", "--json", str(work / "truth.json")])
    truth = truth_book.read_text()
    tj = json.loads((work / "truth.json").read_text())
    outage_ns, outage_label = pick_outage(tj["first_ts"], tj["last_ts"])
    span = tj["last_ts"] - tj["first_ts"]
    # The halt goes in before the outage, so a scenario carrying both puts the
    # gap on the far side of the state change rather than inside it.
    halt_ns = tj["first_ts"] + span * 4 // 10
    checkpoint_ns = tj["first_ts"] + int(span * a.checkpoint_frac)

    # Re-take the truth at the checkpoint rather than at the end of the feed.
    run([str(b / "mold_replay"), str(packets), "--book-out", str(truth_book),
         "--quiet", "--json", str(work / "truth.json"),
         "--book-at-ns", str(checkpoint_ns)])
    truth = truth_book.read_text()
    tj = json.loads((work / "truth.json").read_text())
    print(f"truth: {tj['book_levels']} levels at the checkpoint, "
          f"{tj['messages']:,} messages")
    print(f"outage at {outage_label}, books compared at "
          f"{a.checkpoint_frac:.0%} of the session\n")
    if tj["book_levels"] == 0:
        print("FAIL: the checkpoint book is empty, so every comparison would "
              "match trivially")
        print("and no verdict below would mean anything. Move --checkpoint-frac "
              "to a point")
        print("where the book has orders in it — later than the open, earlier "
              "than the close.")
        return 1

    rows = []
    no_op = []
    for name, flags, must, why in scenarios(a.seed, outage_ns, halt_ns):
        dmg = work / f"{name}.gz"
        report = run([str(b / "mold_damage"), str(packets), str(dmg)] + flags)
        # A scenario that inflicted no damage is not a passing scenario, it is
        # a broken one, and it looks identical to a pass from the verdict alone.
        if must is not None and parse_damage(report.stdout).get(must, 0) == 0:
            no_op.append(f"{name} (nothing was {must})")

        book = work / f"{name}.csv"
        js = work / f"{name}.json"
        # Exit 3 means messages were lost, which is the point of most of these.
        run([str(b / "mold_replay"), str(dmg), "--book-out", str(book), "--quiet",
             "--json", str(js), "--recover-after", str(a.recover_after),
             "--book-at-ns", str(checkpoint_ns)],
            allow=(0, 3))
        st = json.loads(js.read_text())
        matches = book.read_text() == truth
        rows.append({
            "name": name, "why": why, "verdict": verdict(matches, st["trusted"]),
            "lost": st["messages_lost"], "gaps": st["gaps"],
            "dups": st["duplicate_messages"], "reordered": st["reordered_packets"],
            "trunc": st["truncated_packets"], "state": st["state"],
            "rebuilds": st["rebuilds"], "recoveries": st["recoveries"],
        })

    w = max(len(r["name"]) for r in rows)
    print(f"{'scenario':<{w}} {'verdict':>9} {'lost':>8} {'gaps':>6} {'dup':>6} "
          f"{'reord':>6} {'trunc':>6} {'state':>12} {'rebuild':>8} {'recov':>6}")
    print("-" * (w + 76))
    for r in rows:
        print(f"{r['name']:<{w}} {r['verdict']:>9} {r['lost']:>8,} {r['gaps']:>6} "
              f"{r['dups']:>6} {r['reordered']:>6} {r['trunc']:>6} "
              f"{r['state']:>12} {r['rebuilds']:>8} {r['recoveries']:>6}")

    print()
    for r in rows:
        print(f"  {r['name']:<{w}}  {r['why']}")

    pipeline = []
    if a.pipeline:
        print("\n--- the live pipeline: loss caused by backpressure, not by damage ---")
        print("(these compare the ALL-SYMBOLS book, one row per security, not the "
              "single-symbol\n depth dump above — the pipeline builds every symbol "
              "at once)\n")
        pipeline, pipe_problems = pipeline_rows(b, a.feed, packets, work, a)
        no_op += pipe_problems
        pw = max(len(r["name"]) for r in pipeline)
        print(f"{'scenario':<{pw}} {'verdict':>9} {'dropped':>8} {'lost':>8} "
              f"{'gaps':>6} {'state':>12} {'rebuild':>8} {'recov':>6} "
              f"{'peak occ':>9}  kill switch")
        print("-" * (pw + 78))
        for r in pipeline:
            print(f"{r['name']:<{pw}} {r['verdict']:>9} {r['dropped']:>8,} "
                  f"{r['lost']:>8,} {r['gaps']:>6} {r['state']:>12} "
                  f"{r['rebuilds']:>8} {r['recoveries']:>6} {r['peak']:>9,}  "
                  f"{r['kill']}")
        print()
        for r in pipeline:
            print(f"  {r['name']:<{pw}}  {r['why']}")
        rows = rows + pipeline

    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\n" + "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    if a.keep:
        print(f"\nartifacts kept in {work}")
    if tmp is not None:
        tmp.cleanup()

    if no_op:
        print("\nFAIL: these scenarios inflicted no damage, so their verdicts "
              "mean nothing:")
        for n in no_op:
            print(f"  - {n}")
        return 1

    wrong = [r["name"] for r in rows if r["verdict"] == "WRONG"]
    if wrong:
        print("\nFAIL: silently wrong on " + ", ".join(wrong))
        print("The book diverged from the truth while the system reported itself")
        print("trusted. That is the one outcome phase 7 exists to make impossible.")
        return 1
    if rows[0]["verdict"] != "CORRECT":
        print("\nFAIL: the undamaged control did not come back CORRECT — the "
              "harness itself is broken")
        return 1
    # The live table has its own control, and it has to pass for the same
    # reason: a pipeline that cannot reproduce the book with nothing wrong is
    # not a pipeline whose damaged verdicts mean anything.
    for r in pipeline:
        if r["name"] == "pipeline-clean" and r["verdict"] != "CORRECT":
            print("\nFAIL: the unthrottled pipeline did not come back CORRECT — "
                  "its damaged\nverdicts describe a pipeline that is already wrong")
            return 1
    print("\nOK: correct or safe in every scenario, never silently wrong")
    return 0


if __name__ == "__main__":
    sys.exit(main())
