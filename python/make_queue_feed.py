#!/usr/bin/env python3
"""Generate a book-legal ITCH feed with real queue structure.

The other generators are wrong for this phase, in opposite directions.
`make_bench_feed.py` reproduces a real day's message MIX, which is what a
throughput benchmark needs and says nothing about queues. `fuzz_feed.py` is
deliberately adversarial and hits rare paths far too often.

Phase 6 measures what happens to an order sitting in a FIFO, so the feed has to
get the FIFO right:

  * **Executions take from the front.** A real exchange executes the oldest
    order at a price. A generator that executes a random order produces a tape
    no book could have produced, and every queue conclusion drawn from it is
    meaningless.
  * **Cancels have a controllable position.** `--cancel-front-bias` sets the
    fraction of cancels drawn from the front of the queue rather than uniformly.
    This is the knob the whole phase turns on: at 1.0 every cancel is ahead of
    everyone and the optimistic model is correct; at 0.0 they are all at the
    back and the pessimistic model is. Sweeping it is how you find out which
    bound a real day sits near.
  * **Ties.** `--tie-burst` emits several orders at one price and one timestamp,
    which is where arrival-order bugs hide.

It also emits, by construction, the shapes that break naive implementations: a
`C` printing at its resting price and another printing away from it, a
non-printable `C`, a `Q` cross at a tracked price, a halt and resume, and an
opposite-side add that locks a tracked level.

    python3 python/make_queue_feed.py data/raw/queue.gz --seed 1 --messages 20000
"""
import argparse
import gzip
import random
import sys
from collections import deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_sample as gen   # noqa: E402

MID = 100_0000     # $100.0000
TICK = 100
LEVELS = 6         # price levels either side that we actually populate


class Level:
    """A price level's FIFO, mirroring what the book will reconstruct."""

    def __init__(self):
        self.queue = deque()       # refs, oldest first
        self.shares = {}           # ref -> shares

    def add(self, ref, shares):
        self.queue.append(ref)
        self.shares[ref] = shares

    def remove(self, ref):
        self.shares.pop(ref, None)
        try:
            self.queue.remove(ref)
        except ValueError:
            pass

    def total(self):
        return sum(self.shares.values())

    def empty(self):
        return not self.queue


def build(seed, n_messages, cancel_front_bias, tie_burst):
    rng = random.Random(seed)
    out = bytearray()
    t = gen.T0
    match = 1
    next_ref = 1
    # (side, price) -> Level
    levels = {}

    def emit(payload):
        out.extend(gen.frame(payload))

    def lvl(side, price):
        return levels.setdefault((side, price), Level())

    def pick_price(side):
        # Bids below the mid, asks above, so the book stays uncrossed.
        step = rng.randint(1, LEVELS)
        return MID - step * TICK if side == b"B" else MID + step * TICK

    def populated():
        return [k for k, v in levels.items() if not v.empty()]

    emit(gen.system_event(t, b"O"))
    emit(gen.stock_directory(t, gen.SYMBOL))
    emit(gen.system_event(t, b"Q"))

    # Seed both sides so there is a queue to sit in from the start.
    for side in (b"B", b"S"):
        for step in range(1, LEVELS + 1):
            price = MID - step * TICK if side == b"B" else MID + step * TICK
            for _ in range(rng.randint(2, 5)):
                t += rng.randint(1, 20)
                shares = rng.choice([100, 100, 200, 300, 500])
                emit(gen.add_order(t, next_ref, side, shares, price))
                lvl(side, price).add(next_ref, shares)
                next_ref += 1

    emitted = 0
    halted = False
    while emitted < n_messages:
        t += rng.randint(1, 40)
        roll = rng.random()

        # ---- a burst of ties: several adds at one price and one timestamp ----
        if tie_burst and roll < 0.02:
            side = b"B" if rng.random() < 0.5 else b"S"
            price = pick_price(side)
            for _ in range(rng.randint(2, 5)):
                shares = rng.choice([100, 200])
                emit(gen.add_order(t, next_ref, side, shares, price))
                lvl(side, price).add(next_ref, shares)
                next_ref += 1
                emitted += 1
            continue

        # ---- halt and resume ----
        if roll < 0.025:
            emit(gen.trading_action(t, b"H" if not halted else b"T"))
            halted = not halted
            emitted += 1
            continue

        if roll < 0.45:
            # ---- add ----
            side = b"B" if rng.random() < 0.5 else b"S"
            price = pick_price(side)
            shares = rng.choice([100, 100, 200, 300, 500, rng.randint(1, 99)])
            if rng.random() < 0.04:
                emit(gen.add_order_mpid(t, next_ref, side, shares, price, "NSDQ"))
            else:
                emit(gen.add_order(t, next_ref, side, shares, price))
            lvl(side, price).add(next_ref, shares)
            next_ref += 1
            emitted += 1
            continue

        live = populated()
        if not live:
            continue
        side, price = rng.choice(live)
        level = levels[(side, price)]

        if roll < 0.70:
            # ---- execute the FRONT order: this is what price-time priority means ----
            ref = level.queue[0]
            have = level.shares[ref]
            qty = have if rng.random() < 0.6 else rng.randint(1, have)
            if rng.random() < 0.04:
                # A 'C' at the resting price is a trade; one away from it, or a
                # non-printable one, is not. Both shapes appear here on purpose.
                if rng.random() < 0.5:
                    emit(gen.order_executed_price(t, ref, qty, match, b"Y", price))
                else:
                    away = price + rng.choice([-2, -1, 1, 2]) * TICK
                    printable = b"Y" if rng.random() < 0.5 else b"N"
                    emit(gen.order_executed_price(t, ref, qty, match, printable, away))
            else:
                emit(gen.order_executed(t, ref, qty, match))
            match += 1
            if qty >= have:
                level.remove(ref)
            else:
                level.shares[ref] = have - qty
            emitted += 1
            continue

        if roll < 0.92:
            # ---- cancel, with a controllable queue position ----
            if rng.random() < cancel_front_bias:
                ref = level.queue[0]            # ahead of everyone
            else:
                ref = rng.choice(list(level.queue))
            have = level.shares[ref]
            full = rng.random() < 0.8
            if full:
                emit(gen.order_delete(t, ref))
                level.remove(ref)
            else:
                qty = rng.randint(1, max(1, have - 1)) if have > 1 else 1
                emit(gen.order_cancel(t, ref, qty))
                if qty >= have:
                    level.remove(ref)
                else:
                    level.shares[ref] = have - qty
            emitted += 1
            continue

        if roll < 0.96:
            # ---- replace: delete then add, priority lost ----
            ref = rng.choice(list(level.queue))
            new_price = pick_price(side)
            new_shares = rng.choice([100, 200, 300])
            emit(gen.order_replace(t, ref, next_ref, new_shares, new_price))
            level.remove(ref)
            lvl(side, new_price).add(next_ref, new_shares)
            next_ref += 1
            emitted += 1
            continue

        if roll < 0.985:
            # ---- hidden trade: real volume, never in the displayed queue ----
            emit(gen.trade(t, next_ref, b"B", rng.randint(1, 300), price, match))
            match += 1
            emitted += 1
            continue

        # ---- a cross at a populated price ----
        emit(gen.cross_trade(t, rng.randint(1000, 50000), price, match,
                             rng.choice([b"O", b"C"])))
        match += 1
        emitted += 1

    if halted:
        emit(gen.trading_action(t, b"T"))
    emit(gen.system_event(t, b"M"))
    emit(gen.system_event(t, b"C"))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default="data/raw/queue.gz")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--messages", type=int, default=20000)
    ap.add_argument("--cancel-front-bias", type=float, default=0.5,
                    help="fraction of cancels taken from the front of the queue; "
                         "1.0 makes the optimistic model correct, 0.0 the pessimistic one")
    ap.add_argument("--no-tie-burst", action="store_true")
    a = ap.parse_args()

    data = build(a.seed, a.messages, a.cancel_front_bias, not a.no_tie_burst)
    Path(a.path).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(a.path, "wb", compresslevel=1) as f:
        f.write(data)
    print(f"wrote {len(data):,} bytes (~{a.messages:,} messages, seed {a.seed}, "
          f"front-bias {a.cancel_front_bias}) to {a.path}")


if __name__ == "__main__":
    main()
