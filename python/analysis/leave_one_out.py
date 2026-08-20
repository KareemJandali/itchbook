#!/usr/bin/env python3
"""Grade the fill models against orders that really happened.

    python3 python/analysis/leave_one_out.py data/sliced/MSFT.gz \
        --binary build/queue_sim --samples 40 --partial-only

A queue model is a counterfactual: it answers "what would MY order have got",
and no data set contains that answer. Every other check in this phase is
therefore internal — two implementations agreeing, invariants holding, bounds
not crossing. This one is not. It is the only external ground truth available
without a second feed, and it works by shadowing somebody else's order.

The construction, for one real order O:

  1. Find an order added during continuous trading that executed at least once.
     The feed says exactly how many of its shares were filled.
  2. Replay the feed UNCHANGED, with a simulated order of O's price and size
     placed one message before O's add — so it stands exactly where O stood,
     with the same orders ahead of it — and pulled one message before whatever
     removed O.
  3. Compare what each model says we filled against what O actually filled.

Nothing is edited, which is the point. The simulated order never enters the
book, so no other participant's behaviour changes, and the queue ahead of it is
the real queue rather than a reconstruction of one. O itself now sits directly
behind the simulated order, so every execution the tape addresses to O is flow
that reached the front of that queue while we were at the front of it: a model
that consumes what is ahead and then spills onto us reproduces O's fills
exactly, and the ways the four fail to are the measurement.

An earlier version did edit the feed — it deleted O and re-emitted O's
executions as non-displayed prints — and it was wrong in an instructive way.
A hidden print is exactly the message a queue model is built to distrust,
because on real data it is liquidity that never touched the displayed queue.
Disguising displayed executions as hidden ones asks the models to honour
something they are correct to ignore, and all three queue models duly reported
zero fills on every partially filled order. The bug was in the harness, and
the harness is now a replay rather than a rewrite.

What the numbers mean:

  * `bracketed` — the runs where pessimistic <= truth <= optimistic. This is
    the claim the whole phase makes. A truth outside the band means the band
    is not a bound.
  * `mbo` should be near-exact. It resolves references rather than guessing,
    so a large error there is a defect in the resolution layer, not a
    modelling choice.
  * `naive` should over-fill and never under-fill.

Sample the partial fills. An order that ended fully executed is graded against
its own size, and every model reproduces that number as soon as enough flow
arrives, so a run made only of those reports four perfect scores and has
measured nothing. The orders that discriminate are the ones pulled part-filled:
there the answer depends on exactly how far up the queue the order had got,
which is the quantity under test. `--partial-only` selects them, and the
summary always prints the mix so a perfect score can be read with the right
amount of suspicion.

Two honest limits. Our order is one deeper in the level than O was, so its own
size is not part of the liquidity anyone else saw — harmless for the fills we
grade, but it means the run is not a full counterfactual market. And orders
that were replaced ('U') or resized ('X') are skipped, because a replace
continues under a different reference and a resize changes the size the answer
is about, so "what did O get" stops having one answer.
"""
import argparse
import gzip
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from reference import parser as ip              # noqa: E402


class Order:
    __slots__ = ("ref", "index", "ts", "side", "price", "shares", "stock",
                 "executed", "msgs", "replaced", "odd_execution", "cancelled",
                 "tradable_at_add", "death_index")

    def __init__(self, ref, index, ts, side, price, shares, stock):
        self.ref = ref
        self.index = index          # message index of the add
        self.ts = ts
        self.side = side
        self.price = price
        self.shares = shares
        self.stock = stock
        self.executed = 0
        self.msgs = [index]         # every message index that touches this ref
        self.replaced = False
        self.odd_execution = False  # a 'C' away from the resting price, or not printable
        self.cancelled = False      # an 'X' resized it mid-life
        self.tradable_at_add = False
        self.death_index = None     # message index that removed it, if any


def scan(path):
    """One pass: every order's life, and the raw bytes, indexed by position."""
    raw_count = 0
    orders = {}
    live = {}
    tradable = False
    with gzip.open(path, "rb") as f:
        for i, (mtype, p) in enumerate(ip.iter_messages(f)):
            raw_count = i + 1
            if mtype == b"S":
                code = ip.parse_system_event(p)["code"]
                if code == "Q":
                    tradable = True
                elif code in ("M", "C"):
                    tradable = False
            elif mtype in (b"A", b"F"):
                d = ip.parse_add_order(p)
                o = Order(d["ref"], i, ip.read_timestamp(p), d["side"], d["price"],
                          d["shares"], d["stock"])
                o.tradable_at_add = tradable
                orders[d["ref"]] = o
                live[d["ref"]] = o
            elif mtype in (b"E", b"C", b"X", b"D", b"U"):
                d = ip.decode(mtype, p)
                ref = d["orig_ref"] if mtype == b"U" else d["ref"]
                o = live.get(ref)
                if o is None:
                    continue
                o.msgs.append(i)
                if mtype == b"E":
                    o.executed += d["shares"]
                elif mtype == b"C":
                    o.executed += d["shares"]
                    # Either shape breaks the rewrite: a print away from the
                    # resting price never touched this level's displayed queue,
                    # and a non-printable execution puts no volume on the tape
                    # at all, so re-emitting it as a print invents a trade.
                    if d["price"] != o.price or not d["printable"]:
                        o.odd_execution = True
                elif mtype == b"X":
                    o.cancelled = True
                elif mtype == b"U":
                    o.replaced = True
                    o.death_index = i
                    live.pop(ref, None)
                if mtype == b"D":
                    o.death_index = i
                    live.pop(ref, None)
                elif mtype in (b"E", b"C", b"X") and o.executed >= o.shares:
                    o.death_index = i
                    live.pop(ref, None)
    return raw_count, orders


def candidates(orders, min_shares):
    """Orders whose 'what did it get' has exactly one answer.

    Replaced orders are out: a replace continues under a different reference.
    Orders that printed a 'C' away from their resting price are out too — the
    displayed queue at their price never saw that volume, so a print at the
    resting price would misrepresent the tape.
    """
    out = []
    for o in orders.values():
        if o.replaced or o.odd_execution or o.cancelled:
            continue
        if not o.tradable_at_add:
            continue
        if o.executed <= 0 or o.shares < min_shares:
            continue
        out.append(o)
    return out


def spec_for(target):
    """A --place spec that shadows `target`: same price and size, one message
    ahead of its add, pulled one message ahead of whatever removed it.

    queue_sim applies placements after the Nth message, so N = the add's index
    puts us in the book immediately before the add itself lands, which is
    exactly the position the add would have taken. The same arithmetic on the
    death index pulls us just before the order we are shadowing goes away; an
    order that rested to the end of the feed is never pulled, and zero is the
    tool's word for that.
    """
    death = target.death_index
    cancel_after = 0 if death is None else death
    return (f"{'B' if target.side == 'B' else 'S'}:{target.price}:"
            f"{target.shares}:{target.index}:0:{cancel_after}"), cancel_after


def run_sim(binary, feed, spec, json_path):
    r = subprocess.run([binary, str(feed), "--place", spec, "--json", str(json_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"queue_sim failed: {r.stderr.strip()}")
    return json.loads(Path(json_path).read_text())["models"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed")
    ap.add_argument("--binary", default="build/queue_sim")
    ap.add_argument("--samples", type=int, default=25)
    ap.add_argument("--min-shares", type=int, default=100)
    ap.add_argument("--partial-only", action="store_true",
                    help="grade only orders that were pulled part-filled — the "
                         "cases where the answer depends on queue position")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    n_messages, orders = scan(a.feed)
    pool = candidates(orders, a.min_shares)
    if not pool:
        raise SystemExit("error: no usable orders in that feed "
                         "(need adds during continuous trading that executed)")
    partial = [o for o in pool if o.executed < o.shares]
    if a.partial_only:
        pool = partial
        if not pool:
            raise SystemExit("error: no partially filled orders in that feed")
    rng = random.Random(a.seed)
    rng.shuffle(pool)
    pool = pool[:a.samples]
    print(f"{n_messages:,} messages, {len(orders):,} orders, "
          f"grading {len(pool)} of them")
    n_partial = sum(1 for o in pool[:a.samples] if o.executed < o.shares)
    print(f"{n_partial} of the graded orders were pulled part-filled "
          f"(the discriminating cases); {len(partial)} exist in the feed\n")

    models = ["naive", "optimistic", "mbo", "pessimistic"]
    err = {m: [] for m in models}
    truth_total = 0
    bracketed = 0
    with tempfile.TemporaryDirectory() as tmp:
        json_path = Path(tmp) / "loo.json"
        for k, o in enumerate(pool):
            spec, cancel_after = spec_for(o)
            got = run_sim(a.binary, a.feed, spec, json_path)
            truth = o.executed
            truth_total += truth
            for m in models:
                err[m].append(got[m]["shares"] - truth)
            lo, hi = got["pessimistic"]["shares"], got["optimistic"]["shares"]
            if lo <= truth <= hi:
                bracketed += 1
            if a.verbose:
                cells = "  ".join(f"{m[:4]}={got[m]['shares']:>6}" for m in models)
                death = "end" if cancel_after == 0 else str(cancel_after)
                print(f"  [{k:>3}] ref {o.ref:<10} {o.side} "
                      f"{o.price / 10000:>9.4f} x{o.shares:<6} truth={truth:<6} "
                      f"place={o.index} pull={death}  {cells}")

    n = len(pool)
    print(f"\n{'model':<14}{'mean error':>12}{'mean |error|':>14}"
          f"{'over':>7}{'under':>7}{'exact':>7}")
    print("-" * 61)
    for m in models:
        e = err[m]
        print(f"{m:<14}{sum(e) / n:>12.1f}{sum(abs(v) for v in e) / n:>14.1f}"
              f"{sum(1 for v in e if v > 0):>7}{sum(1 for v in e if v < 0):>7}"
              f"{sum(1 for v in e if v == 0):>7}")
    print(f"\ntruth: {truth_total:,} shares over {n} orders "
          f"({truth_total / n:.0f} per order)")
    print(f"bracketed by [pessimistic, optimistic]: {bracketed}/{n} "
          f"({100.0 * bracketed / n:.0f}%)")

    # The falsifiable statements, checked rather than asserted in prose.
    fails = []
    if any(v < 0 for v in err["naive"]):
        fails.append("naive under-filled at least once; it ignores the queue "
                     "and cannot fill less than a model that waits its turn")
    if bracketed < n:
        fails.append(f"{n - bracketed} orders fell outside "
                     f"[pessimistic, optimistic]; the band is not a bound")
    if fails:
        print("\nFAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("\nOK: every real order landed inside the band the models claim for it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
