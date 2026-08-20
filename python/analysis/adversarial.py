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

The comparison is the whole final book — every level, both sides, price and
shares and order count — not top-of-book and not a summary total. Two books
that differ only deep in the tail are two different books, and a harness that
compares totals would report a pass while the thing under test was broken in
exactly the place a maker rests.
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
]


def scenarios(seed, outage_ns):
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
                    help="consecutive clean references before the book is trusted "
                         "again")
    ap.add_argument("--keep", metavar="DIR",
                    help="keep the damaged feeds and books here instead of a "
                         "temporary directory")
    a = ap.parse_args()

    b = Path(a.build)
    for tool in ("mold_wrap", "mold_damage", "mold_replay"):
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
    print(f"truth: {len(truth.splitlines()) - 1} levels, "
          f"{tj['messages']:,} messages")
    print(f"outage scheduled at {outage_label}\n")

    rows = []
    no_op = []
    for name, flags, must, why in scenarios(a.seed, outage_ns):
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
             "--json", str(js), "--recover-after", str(a.recover_after)],
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
    print("\nOK: correct or safe in every scenario, never silently wrong")
    return 0


if __name__ == "__main__":
    sys.exit(main())
