#!/usr/bin/env python3
"""Generate a large, adversarial-but-valid ITCH 5.0 feed for differential testing.

`make_sample.py` produces a small, tidy, spec-shaped feed — good for smoke tests
and for asserting hand-computed numbers, useless for finding the bugs that only
appear under churn. This produces the opposite: tens of thousands of messages
that deliberately hit the paths a tidy feed never reaches.

    python3 python/fuzz_feed.py data/raw/fuzz.gz --seed 1 --messages 50000

What it aims at, and why each one matters to the C++ implementation:

  * prices outside the dense tick band       -> the cold overflow map
  * prices off the tick grid (sub-penny)     -> overflow again; two off-grid
                                                prices must not collapse into
                                                one dense slot
  * levels emptied and refilled repeatedly   -> the best bid/ask cursor walk
  * heavy add/delete churn                   -> the ref map's backward-shift
                                                deletion, which is where a
                                                tombstone implementation rots
  * executions larger than the resting size  -> unsigned underflow in C++
  * messages naming refs that never existed  -> the unknown-ref path
  * a second symbol on another stock locate  -> the symbol filter

Everything emitted is a structurally valid ITCH message with a correct length
prefix; this fuzzes the book, not the framing. Given the same seed it produces
the same bytes, so a failing case is reproducible.
"""
import argparse
import gzip
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_sample as gen   # noqa: E402

MID = gen.MID
TICK = gen.TICK
OTHER_LOC = 7          # a second symbol, to exercise the locate filter


def build(seed, n_messages):
    rng = random.Random(seed)
    out = bytearray()
    t = gen.T0
    match = 1
    next_ref = 1
    live = {}              # ref -> shares, for the target symbol only
    # A sampling pool over `live`. Refs are appended on add and left behind when
    # an order dies, then evicted lazily when drawn — picking is amortised O(1).
    # Rebuilding a list of live refs per message instead makes generating a
    # realistically sized feed quadratic, which is how this started out.
    live_refs = []

    def add_live(ref, shares):
        live[ref] = shares
        live_refs.append(ref)

    def pick_live():
        while live_refs:
            i = rng.randrange(len(live_refs))
            ref = live_refs[i]
            if ref in live:
                return ref
            live_refs[i] = live_refs[-1]
            live_refs.pop()
        return None

    def emit(payload):
        out.extend(gen.frame(payload))

    def other_locate(payload):
        """Re-stamp a payload onto the second symbol's stock locate."""
        p = bytearray(payload)
        p[1:3] = OTHER_LOC.to_bytes(2, "big")
        return bytes(p)

    def pick_price():
        r = rng.random()
        if r < 0.80:
            # The common case: a tick inside the band, near the touch.
            return MID + rng.randint(-40, 40) * TICK
        if r < 0.90:
            # Far outside the +/-20% band -> overflow map.
            return MID + rng.choice([-1, 1]) * rng.randint(3000, 9000) * TICK
        if r < 0.97:
            # Off the tick grid -> overflow map, and must not collide.
            return MID + rng.randint(-40, 40) * TICK + rng.randint(1, 99)
        # Pathological but legal: a stub quote at a penny.
        return rng.randint(1, 200)

    emit(gen.system_event(t, b"O"))
    emit(gen.stock_directory(t, gen.SYMBOL))
    emit(other_locate(gen.stock_directory(t, "OTHR")))
    emit(gen.system_event(t, b"Q"))

    emitted = 0
    while emitted < n_messages:
        t += rng.randint(1, 5_000)
        roll = rng.random()

        # A message on the other symbol: the book under test must ignore it.
        if roll < 0.05:
            ref = next_ref
            next_ref += 1
            emit(other_locate(gen.add_order(t, ref, b"B", rng.randint(1, 500),
                                            pick_price(), "OTHR")))
            emitted += 1
            continue

        # Adds dominate a real feed, so they dominate here.
        if roll < 0.45 or not live:
            ref = next_ref
            next_ref += 1
            shares = rng.randint(1, 1000)
            side = b"B" if rng.random() < 0.5 else b"S"
            price = pick_price()
            if rng.random() < 0.1:
                emit(gen.add_order_mpid(t, ref, side, shares, price, "NSDQ"))
            else:
                emit(gen.add_order(t, ref, side, shares, price))
            add_live(ref, shares)
            emitted += 1
            continue

        # One in fifty mutations names a reference that never existed.
        if rng.random() < 0.02:
            ghost = next_ref + 1_000_000
            choice = rng.choice(["E", "X", "D", "U"])
            if choice == "E":
                emit(gen.order_executed(t, ghost, 10, match)); match += 1
            elif choice == "X":
                emit(gen.order_cancel(t, ghost, 10))
            elif choice == "D":
                emit(gen.order_delete(t, ghost))
            else:
                emit(gen.order_replace(t, ghost, next_ref, 10, pick_price()))
                next_ref += 1
            emitted += 1
            continue

        ref = pick_live()
        if ref is None:
            continue
        shares = live[ref]
        action = rng.random()

        if action < 0.30:
            # Execute. Sometimes for more than is resting, which must remove the
            # order rather than wrap an unsigned counter.
            qty = rng.randint(1, shares) if rng.random() < 0.85 else shares + rng.randint(1, 50)
            emit(gen.order_executed(t, ref, qty, match)); match += 1
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        elif action < 0.45:
            # Execute at a stated price; a third of these are non-printable.
            qty = rng.randint(1, shares)
            printable = b"Y" if rng.random() < 0.67 else b"N"
            emit(gen.order_executed_price(t, ref, qty, match, printable, pick_price()))
            match += 1
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        elif action < 0.65:
            qty = rng.randint(1, shares)
            emit(gen.order_cancel(t, ref, qty))
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        elif action < 0.85:
            emit(gen.order_delete(t, ref))
            del live[ref]
        elif action < 0.95:
            new_ref = next_ref
            next_ref += 1
            new_shares = rng.randint(1, 1000)
            emit(gen.order_replace(t, ref, new_ref, new_shares, pick_price()))
            del live[ref]
            add_live(new_ref, new_shares)
        elif action < 0.98:
            emit(gen.trade(t, next_ref, b"B", rng.randint(1, 500), pick_price(), match))
            match += 1
        else:
            emit(gen.cross_trade(t, rng.randint(1, 10000), pick_price(), match,
                                 rng.choice([b"O", b"C", b"H", b"I"])))
            match += 1
        emitted += 1

    emit(gen.trading_action(t, b"T"))
    emit(gen.system_event(t, b"M"))
    emit(gen.system_event(t, b"C"))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default="data/raw/fuzz.gz")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--messages", type=int, default=50_000)
    a = ap.parse_args()

    data = build(a.seed, a.messages)
    Path(a.path).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(a.path, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes (seed {a.seed}, ~{a.messages} messages) to {a.path}")


if __name__ == "__main__":
    main()
