#!/usr/bin/env python3
"""Generate a book-legal ITCH feed with real queue structure.

The other generators are wrong for this phase, in opposite directions.
`make_bench_feed.py` reproduces a real day's message MIX, which is what a
throughput benchmark needs and says nothing about queues. `fuzz_feed.py` is
deliberately adversarial and hits rare paths far too often.

Phase 6 measures what happens to an order sitting in a FIFO, so the feed has to
get the FIFO right:

  * **Executions take from the front, at the touch.** A real exchange executes
    the oldest order at the BEST price on that side; both halves matter. A
    generator that executes a random order at a random level produces a tape no
    single venue could have produced — a print below a resting bid — and every
    queue conclusion drawn from it is meaningless. The leave-one-out oracle
    found this the hard way: with executions scattered across levels, price
    priority fired on nearly every simulated fill, and a replacement order
    filled 100 shares where the real order it replaced had filled 23.
  * **Cancels have a controllable position.** `--cancel-front-bias` sets the
    fraction of cancels drawn from the front of the queue rather than uniformly.
    This is the knob the whole phase turns on: at 1.0 every cancel is ahead of
    everyone and the optimistic model is correct; at 0.0 they are all at the
    back and the pessimistic model is. Sweeping it is how you find out which
    bound a real day sits near.
  * **Ties.** `--tie-burst` emits several orders at one price and one timestamp,
    which is where arrival-order bugs hide.
  * **The price moves, and it moves for the right reason.** `--drift-prob` sets
    how often the centre takes a one-tick step, and a step is not a relabelling:
    the touch on the consumed side is executed out of existence first, so the
    price moves because someone took the liquidity resting at it. A generator
    with a pinned centre — which is what this one used to be — produces a feed
    where every fill markouts to exactly zero, no quote is ever picked off, and
    the four fill models cannot disagree, because nothing that would separate
    them ever happens.

Halts are real halts. Nothing executes while the symbol is halted — orders can
still be entered and pulled, which is what a halt is for, but no trade prints
and the centre does not move. Emitting executions through a halt desynchronises
every model from the book on purpose: the model correctly ignores messages it
is told cannot be trading, its idea of what is ahead stops matching reality,
and it then reports zero fills for the rest of the order's life. Halts are also
rare and bounded, rather than a coin flip on every message that left the feed
halted about half the time.

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


def build(seed, n_messages, cancel_front_bias, tie_burst, gap_ns, drift_prob):
    rng = random.Random(seed)
    out = bytearray()
    t = gen.T0
    match = 1
    next_ref = 1
    center = MID              # the price the book is currently built around
    # (side, price) -> Level
    levels = {}

    def emit(payload):
        out.extend(gen.frame(payload))

    def advance():
        # Bursty, like a real feed: mostly tight, occasionally idle. The scale
        # matters — markout horizons are 100ms/1s/10s, so a feed that spans
        # microseconds can never resolve one.
        if rng.random() < 0.9:
            return rng.randint(1, gap_ns)
        return rng.randint(gap_ns, gap_ns * 40)

    def lvl(side, price):
        return levels.setdefault((side, price), Level())

    def pick_price(side):
        # Bids below the centre, asks above, so the book stays uncrossed
        # wherever the centre currently is.
        step = rng.randint(1, LEVELS)
        return center - step * TICK if side == b"B" else center + step * TICK

    def populated():
        return [k for k, v in levels.items() if not v.empty()]

    def touch(side):
        """The best price on one side, or None if that side is empty.

        Executions may only happen here. A displayed order at a better price
        has to trade first, so a print at a worse level while the touch is
        populated is a tape no single venue produces.
        """
        prices = [p for (sd, p) in populated() if sd == side]
        if not prices:
            return None
        return max(prices) if side == b"B" else min(prices)

    def move_center(direction):
        """Step the centre one tick, by consuming what stood in the way.

        The messages emitted here are ordinary executions, because that is what
        a price move IS on this feed: the resting queue at the touch is taken
        out and the next level becomes the touch. Anyone sitting in that queue
        — including the simulated order — is filled immediately before the
        price moves away from them, which is the entire adverse-selection
        effect this phase measures. Relabelling the centre without emitting the
        executions would move the price for free and hand every maker a profit.

        Everything on the wrong side of the new centre is cleared, not just the
        touch, so the book cannot cross when the centre walks back over levels
        it left behind earlier.
        """
        nonlocal center, match, emitted
        new_center = center + direction * TICK
        for (sd, px), level in list(levels.items()):
            if level.empty():
                continue
            if not ((sd == b"S" and px <= new_center) or (sd == b"B" and px >= new_center)):
                continue
            while not level.empty():
                ref = level.queue[0]
                emit(gen.order_executed(t, ref, level.shares[ref], match))
                match += 1
                level.remove(ref)
                emitted += 1
        center = new_center

    emit(gen.system_event(t, b"O"))
    emit(gen.stock_directory(t, gen.SYMBOL))
    emit(gen.system_event(t, b"Q"))

    # Seed both sides so there is a queue to sit in from the start.
    for side in (b"B", b"S"):
        for step in range(1, LEVELS + 1):
            price = MID - step * TICK if side == b"B" else MID + step * TICK
            for _ in range(rng.randint(2, 5)):
                t += advance()
                shares = rng.choice([100, 100, 200, 300, 500])
                emit(gen.add_order(t, next_ref, side, shares, price))
                lvl(side, price).add(next_ref, shares)
                next_ref += 1

    emitted = 0
    halted = False
    resume_at = 0
    while emitted < n_messages:
        t += advance()

        # ---- resume from a halt ----
        if halted and emitted >= resume_at:
            emit(gen.trading_action(t, b"T"))
            halted = False
            emitted += 1
            continue

        # ---- the centre takes a step ----
        # First, before any other branch, so the executions that move the price
        # carry that message's timestamp and a maker at the touch is filled at
        # the moment of the move rather than some events later. Not while
        # halted: a halted symbol does not trade, and the centre only moves
        # because someone traded through it.
        if not halted and rng.random() < drift_prob:
            move_center(1 if rng.random() < 0.5 else -1)
            continue

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

        # ---- go into a halt ----
        if not halted and roll < 0.0006:
            emit(gen.trading_action(t, b"H"))
            halted = True
            resume_at = emitted + rng.randint(40, 200)
            emitted += 1
            continue

        # While halted the book still moves — orders are entered and pulled —
        # but nothing trades. Redraw into the three branches that do not print
        # a trade, rather than letting a roll fall through into one that does.
        if halted:
            roll = rng.choice([rng.uniform(0.00, 0.45),    # add
                               rng.uniform(0.70, 0.92),    # cancel
                               rng.uniform(0.92, 0.96)])   # replace

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
            # ---- execute the FRONT order AT THE TOUCH: price-time priority ----
            # Both halves. Time priority picks the oldest order at the price;
            # price priority says the price can only be the best one on that
            # side. Executing a deep level while the touch is populated is the
            # single most damaging thing this generator could get wrong, because
            # the simulator would then be right to report a price-priority fill
            # on flow that no real book would have produced.
            best = touch(side)
            if best is None:
                continue
            price = best
            level = levels[(side, price)]
            if level.empty():
                continue
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
            # Placed at or inside the spread, which is where non-displayed
            # interest actually sits (midpoint pegs above all). Scattering it
            # across the book instead makes prints land strictly through resting
            # quotes constantly, which fires price priority on nearly every fill
            # and drowns out the queue effect the phase exists to measure.
            best_bid = max((p for (sd, p) in populated() if sd == b"B"), default=MID - TICK)
            best_ask = min((p for (sd, p) in populated() if sd == b"S"), default=MID + TICK)
            if best_ask - best_bid >= 2 * TICK:
                hidden_price = rng.randint(best_bid + 1, best_ask - 1)
            else:
                hidden_price = rng.choice([best_bid, best_ask])
            emit(gen.trade(t, next_ref, b"B", rng.randint(1, 300), hidden_price, match))
            match += 1
            emitted += 1
            continue

        # ---- a cross, printed at the touch ----
        # A cross prints at one price for everybody. Printing it at a random
        # populated level would put an auction print below resting bids, which
        # is the same priority violation as a stray execution.
        cross_px = touch(side)
        if cross_px is None:
            continue
        emit(gen.cross_trade(t, rng.randint(1000, 50000), cross_px, match,
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
    ap.add_argument("--drift-prob", type=float, default=0.004,
                    help="per-message chance the centre takes a one-tick step, "
                         "consuming the touch on its way (0 pins the price and "
                         "makes every markout identically zero)")
    ap.add_argument("--gap-ns", type=int, default=200000,
                    help="typical inter-message gap in nanoseconds (default 200us, so "
                         "20k messages span a few seconds and markout horizons resolve)")
    a = ap.parse_args()

    data = build(a.seed, a.messages, a.cancel_front_bias, not a.no_tie_burst,
                 a.gap_ns, a.drift_prob)
    Path(a.path).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(a.path, "wb", compresslevel=1) as f:
        f.write(data)
    print(f"wrote {len(data):,} bytes (~{a.messages:,} messages, seed {a.seed}, "
          f"front-bias {a.cancel_front_bias}, drift {a.drift_prob}) to {a.path}")


if __name__ == "__main__":
    main()
