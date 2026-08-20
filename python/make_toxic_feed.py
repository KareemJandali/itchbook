#!/usr/bin/env python3
"""A feed engineered so that every fill is adverse, by exactly two ticks.

    python3 python/make_toxic_feed.py data/raw/toxic.gz --episodes 40
    ./build/queue_backtest data/raw/toxic.gz --strategy touch-maker \
        --json toxic.json
    python3 python/make_toxic_feed.py --check toxic.json

The other feeds are random and the backtester's answers on them can only be
compared against each other. This one has a closed form. Every quantity the
ledger and the markout engine report is fixed by construction before the
simulator runs, so disagreement is a defect and not an opinion:

    edge          = +1.0000 cents/share   (a two-tick spread, quoted at the touch)
    drift, all h  = -2.0000 cents/share   (the mid moves two ticks against us)
    edge + drift  = -1.0000 cents/share   (what each fill was worth)
    gross P&L     =  0 exactly            (see below — this is not a contradiction)
    residual      = 0                     (episodes alternate side)
    unresolved    = 0                     (every fill has 10s of feed after it)

Those last two lines are the interesting pair, and mistaking one for the other
is the reason this feed is worth having. Every individual fill is worth minus
one cent, measured honestly against the mid ten seconds later. The realized
round trip is nevertheless exactly zero before fees: we buy the bid at
mid - 1 tick, the mid drops two ticks, and the ask we then sell is the new
mid + 1 tick — the same price we bought at. A two-tick spread against a
two-tick adverse move nets zero, which is the textbook statement of what
adverse selection does to market making, and the only thing left is the
rebate. A markout and a P&L answer different questions; a backtester that
cannot show both is hiding one of them.

and all four fill models must report identical numbers, because the queue is
fully determined: nothing is ambiguous for a model to be optimistic or
pessimistic about.

That last line is the point of the whole exercise. A backtester whose four
models disagree here is finding queue ambiguity in a feed that has none, which
means the ambiguity is coming from the implementation.

One episode, on the bid (the ask is the mirror, and they alternate so the
position and the price both return to where they started):

    T+0ms    add a 300-share order behind us
    T+10ms   execute the order ahead of us, in full  -> nothing is ahead
    T+20ms   execute 100 of the order behind us      -> time priority fills US
    T+30ms   the mid drops two ticks
    +0.2s, +2s, +12s   quiet messages, so 100ms/1s/10s all resolve

Three details are load-bearing and each of them was a bug first:

  * **The level never empties.** The order behind us is bigger than the
    execution that reaches it, so the best bid is still at our price when we
    fill. If the level emptied, the mid at the fill instant would be computed
    from the next level down and the edge would come out zero.
  * **Every execution takes the front of its queue.** The order ahead of us
    is fully executed before the one behind it is touched. A tape that
    executes out of order is not one a real book could have produced.
  * **The move happens after the fill, not with it.** The fill prints at the
    old mid and the move lands 10ms later, so all three markout horizons see
    the new price and none of them straddles the transition.
"""
import argparse
import gzip
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_sample as gen   # noqa: E402

MID = 100_0000       # $100.0000
TICK = 100           # $0.0100
FLOOR = 50 * TICK    # deep levels, so the book is never one-sided
QUOTE = 100          # the size the strategy is expected to quote
BEHIND = 300         # the order behind us: bigger than what reaches it

MS = 1_000_000
S = 1_000_000_000

# The construction fixes these. They are cents per share.
EXPECTED_EDGE_CENTS = 1.0
EXPECTED_DRIFT_CENTS = -2.0
# Buy price and sell price are identical by construction, so everything the
# strategy earns or loses is fees.
EXPECTED_GROSS_MICROS = 0


def build(episodes):
    out = bytearray()
    t = gen.T0
    ref = 1
    match = 1

    def emit(payload):
        out.extend(gen.frame(payload))

    def new_ref():
        nonlocal ref
        ref += 1
        return ref - 1

    emit(gen.system_event(t, b"O"))
    emit(gen.stock_directory(t, gen.SYMBOL))
    emit(gen.system_event(t, b"Q"))

    mid = MID
    # Deep floors, never touched. Without them a moment when the near level is
    # empty leaves the book one-sided and the mid undefined, and a strategy that
    # cannot see a mid stops quoting.
    emit(gen.add_order(t, new_ref(), b"B", 10000, mid - FLOOR))
    emit(gen.add_order(t, new_ref(), b"S", 10000, mid + FLOOR))

    # The near levels. One order each side, so the queue ahead of the strategy's
    # quote is exactly this size and every model computes the same number.
    near_bid = new_ref()
    near_ask = new_ref()
    emit(gen.add_order(t, near_bid, b"B", QUOTE, mid - TICK))
    emit(gen.add_order(t, near_ask, b"S", QUOTE, mid + TICK))

    # A beat, so the strategy's first quote has arrived and joined the queue
    # before anything trades.
    t += S
    emit(gen.trading_action(t, b"T"))

    for i in range(episodes):
        buy_side = (i % 2 == 0)
        side = b"B" if buy_side else b"S"
        price = mid - TICK if buy_side else mid + TICK
        ahead_ref = near_bid if buy_side else near_ask

        # T+0: the order behind us. Added before anything trades, so the level
        # cannot empty at the instant we fill.
        t += 10 * MS
        behind_ref = new_ref()
        emit(gen.add_order(t, behind_ref, side, BEHIND, price))

        # T+10ms: the order ahead of us goes, in full. Front of the queue.
        t += 10 * MS
        emit(gen.order_executed(t, ahead_ref, QUOTE, match))
        match += 1

        # T+20ms: 100 shares of the order behind us. It is behind us, so time
        # priority says we trade first — this is the fill under test.
        t += 10 * MS
        emit(gen.order_executed(t, behind_ref, QUOTE, match))
        match += 1

        # T+30ms: the price moves two ticks against whoever just filled.
        # Deletes first, then the new near levels, so the book is never locked
        # in the middle of the step.
        t += 10 * MS
        emit(gen.order_delete(t, behind_ref))
        emit(gen.order_delete(t, near_ask if buy_side else near_bid))
        mid = mid - 2 * TICK if buy_side else mid + 2 * TICK
        near_bid = new_ref()
        near_ask = new_ref()
        emit(gen.add_order(t, near_bid, b"B", QUOTE, mid - TICK))
        emit(gen.add_order(t, near_ask, b"S", QUOTE, mid + TICK))

        # Quiet, past every markout horizon, so all three resolve at the new
        # price and none is left unresolved.
        for gap in (200 * MS, 2 * S, 12 * S):
            t += gap
            emit(gen.trading_action(t, b"T"))

    emit(gen.system_event(t, b"M"))
    emit(gen.system_event(t, b"C"))
    return bytes(out)


def check(path, episodes):
    d = json.loads(Path(path).read_text())
    models = d["models"]
    print(f"strategy: {d.get('strategy', '?')}   events: {d.get('events', 0):,}\n")
    print(f"{'model':<14}{'fills':>7}{'shares':>9}{'edge c/sh':>12}"
          f"{'drift 100ms':>13}{'drift 1s':>11}{'drift 10s':>11}"
          f"{'unres':>7}{'resid':>8}")
    print("-" * 92)
    rows = {}
    for name, m in models.items():
        shares = m["shares"] or 1
        rows[name] = {
            "fills": m["fills"],
            "shares": m["shares"],
            "edge": m["edge_micros"] / shares / 10000.0,
            "d100": m["drift_100ms"] / 10000.0,
            "d1s": m["drift_1s"] / 10000.0,
            "d10s": m["drift_10s"] / 10000.0,
            "unresolved": m.get("unresolved_10s", 0),
            "residual": m["residual"],
            # equity is net of fees and fees are positive-is-a-cost, so the
            # gross is the sum rather than the difference.
            "gross": m["equity_micros"] + m["fees_micros"],
        }
        r = rows[name]
        print(f"{name:<14}{r['fills']:>7}{r['shares']:>9}{r['edge']:>12.4f}"
              f"{r['d100']:>13.4f}{r['d1s']:>11.4f}{r['d10s']:>11.4f}"
              f"{r['unresolved']:>7}{r['residual']:>8}")

    fails = []
    ref = None
    for name, r in rows.items():
        if abs(r["edge"] - EXPECTED_EDGE_CENTS) > 1e-9:
            fails.append(f"{name}: edge {r['edge']:+.4f} c/share, "
                         f"expected {EXPECTED_EDGE_CENTS:+.4f}")
        for key, label in (("d100", "100ms"), ("d1s", "1s"), ("d10s", "10s")):
            if abs(r[key] - EXPECTED_DRIFT_CENTS) > 1e-9:
                fails.append(f"{name}: drift at {label} {r[key]:+.4f} c/share, "
                             f"expected {EXPECTED_DRIFT_CENTS:+.4f}")
        if r["unresolved"] != 0:
            fails.append(f"{name}: {r['unresolved']} fills had no 10s future, "
                         f"but every episode is followed by 14 seconds of feed")
        if r["gross"] != EXPECTED_GROSS_MICROS:
            fails.append(f"{name}: gross P&L {r['gross']} micros, expected 0 — "
                         f"every buy and the sell that unwinds it are at the "
                         f"same price")
        if r["residual"] != 0:
            fails.append(f"{name}: residual position {r['residual']}, "
                         f"but the episodes alternate side and must net to flat")
        if episodes is not None and r["fills"] != episodes:
            fails.append(f"{name}: {r['fills']} fills, expected one per episode "
                         f"({episodes})")
        # Nothing here is ambiguous, so nothing here may differ by model.
        if ref is None:
            ref = r
        elif r != ref:
            fails.append(f"{name}: differs from the first model on a feed with "
                         f"no queue ambiguity in it")

    print()
    if fails:
        print("FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(f"OK: edge {EXPECTED_EDGE_CENTS:+.4f}, drift {EXPECTED_DRIFT_CENTS:+.4f} "
          f"c/share at every horizon, gross P&L exactly zero, flat at the end,")
    print("    and all four models agree — there is no queue ambiguity here to "
          "disagree about")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default="data/raw/toxic.gz")
    ap.add_argument("--episodes", type=int, default=40,
                    help="one fill each, alternating side (default 40)")
    ap.add_argument("--check", metavar="RESULTS.JSON",
                    help="verify a queue_backtest --json run against the "
                         "closed form instead of writing a feed")
    a = ap.parse_args()

    if a.check:
        return check(a.check, a.episodes)

    data = build(a.episodes)
    Path(a.path).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(a.path, "wb", compresslevel=1) as f:
        f.write(data)
    print(f"wrote {len(data):,} bytes, {a.episodes} episodes, to {a.path}")
    print(f"expected per share: edge {EXPECTED_EDGE_CENTS:+.4f}c, "
          f"drift {EXPECTED_DRIFT_CENTS:+.4f}c at every horizon, "
          f"{EXPECTED_EDGE_CENTS + EXPECTED_DRIFT_CENTS:+.4f}c per fill, "
          f"and a gross round-trip P&L of exactly zero")
    return 0


if __name__ == "__main__":
    sys.exit(main())
