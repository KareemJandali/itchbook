#!/usr/bin/env python3
"""Generate a benchmark feed with the message mix of a real trading day.

Benchmarking against `fuzz_feed.py` would measure the wrong thing: that
generator is deliberately adversarial, so it over-weights the rare paths a
differential test wants to hit and under-weights the two message types that
dominate a real feed. Optimising against it would tune the cold paths.

The distribution here is the one measured from MSFT on 2019-12-30 — the day
validated in `validation/` — 1,221,484 messages for the symbol:

    A  573,851   46.99%    add order
    D  548,858   44.95%    delete
    U   54,873    4.49%    replace
    E   34,739    2.84%    execute
    P    5,057    0.41%    hidden trade
    F    2,175    0.18%    add with attribution
    X      840    0.07%    partial cancel
    C      576    0.05%    execute with price

Add and delete are 92% of the feed between them. That single fact is what
should drive dispatch ordering, and it is why this file exists.

The price shape is drawn from the same day: mostly within a few ticks of the
touch, a long tail out to stub quotes at $0.0001 and $199,999.99, and about
0.15% of prices off the penny grid — the real day had 1,842 of those, and they
are what exercises the book's overflow map.

    python3 python/make_bench_feed.py data/raw/bench.gz --messages 2000000
"""
import argparse
import gzip
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_sample as gen   # noqa: E402

MID = 157_5000          # roughly where MSFT sat that day, in Price(4)
TICK = 100

# Cumulative weights over the book-mutating types, from the census above.
WEIGHTS = [
    ("A", 0.46999),
    ("D", 0.44952),
    ("U", 0.04494),
    ("E", 0.02845),
    ("P", 0.00414),
    ("F", 0.00178),
    ("X", 0.00069),
    ("C", 0.00047),
]


def build(seed, n_messages):
    rng = random.Random(seed)
    out = bytearray()
    t = gen.T0
    match = 1
    next_ref = 1
    live = {}
    live_refs = []

    types = [k for k, _ in WEIGHTS]
    cum = []
    acc = 0.0
    for _, w in WEIGHTS:
        acc += w
        cum.append(acc)
    total_w = acc

    def emit(payload):
        out.extend(gen.frame(payload))

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

    def pick_price():
        r = rng.random()
        if r < 0.94:
            # Near the touch: this is where the hot levels live.
            return MID + rng.randint(-50, 50) * TICK
        if r < 0.985:
            return MID + rng.randint(-800, 800) * TICK
        if r < 0.9985:
            # Off the penny grid — 0.15%, as measured.
            return MID + rng.randint(-50, 50) * TICK + rng.randint(1, 99)
        # Stub quotes at the extremes of the price range.
        return rng.choice([1, 1999999900])

    def pick_type():
        r = rng.random() * total_w
        for i, c in enumerate(cum):
            if r <= c:
                return types[i]
        return "A"

    emit(gen.system_event(t, b"O"))
    emit(gen.stock_directory(t, gen.SYMBOL))
    emit(gen.system_event(t, b"Q"))

    emitted = 0
    while emitted < n_messages:
        t += rng.randint(1, 200)
        kind = pick_type()

        if kind in ("A", "F") or not live:
            ref = next_ref
            next_ref += 1
            shares = rng.choice([100, 100, 100, 200, 300, 500, 1000, rng.randint(1, 99)])
            side = b"B" if rng.random() < 0.5 else b"S"
            price = pick_price()
            if kind == "F":
                emit(gen.add_order_mpid(t, ref, side, shares, price, "NSDQ"))
            else:
                emit(gen.add_order(t, ref, side, shares, price))
            add_live(ref, shares)
            emitted += 1
            continue

        if kind == "P":
            emit(gen.trade(t, next_ref, b"B", rng.randint(1, 500), pick_price(), match))
            match += 1
            emitted += 1
            continue

        ref = pick_live()
        if ref is None:
            continue
        shares = live[ref]

        if kind == "D":
            emit(gen.order_delete(t, ref))
            del live[ref]
        elif kind == "U":
            new_ref = next_ref
            next_ref += 1
            new_shares = rng.choice([100, 200, 300, rng.randint(1, 500)])
            emit(gen.order_replace(t, ref, new_ref, new_shares, pick_price()))
            del live[ref]
            add_live(new_ref, new_shares)
        elif kind == "E":
            qty = shares if rng.random() < 0.5 else rng.randint(1, shares)
            emit(gen.order_executed(t, ref, qty, match))
            match += 1
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        elif kind == "X":
            qty = rng.randint(1, shares)
            emit(gen.order_cancel(t, ref, qty))
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        elif kind == "C":
            qty = rng.randint(1, shares)
            printable = b"Y" if rng.random() < 0.9 else b"N"
            emit(gen.order_executed_price(t, ref, qty, match, printable, pick_price()))
            match += 1
            if qty >= shares:
                del live[ref]
            else:
                live[ref] = shares - qty
        emitted += 1

    emit(gen.system_event(t, b"M"))
    emit(gen.system_event(t, b"C"))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default="data/raw/bench.gz")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--messages", type=int, default=2_000_000)
    a = ap.parse_args()

    data = build(a.seed, a.messages)
    Path(a.path).parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(a.path, "wb", compresslevel=1) as f:
        f.write(data)
    print(f"wrote {len(data):,} bytes (~{a.messages:,} messages, seed {a.seed}) to {a.path}")


if __name__ == "__main__":
    main()
