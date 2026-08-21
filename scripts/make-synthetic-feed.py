#!/usr/bin/env python3
"""make-synthetic-feed.py — a small ITCH 5.0 feed that owes nothing to NASDAQ.

The daily sample file is 3.3 GB, gitignored, and not on the CI runner. Every
tool in this repository that reads a feed has therefore been testable only on
the one machine that has the data, which means the pipeline tools land untested
until someone runs them by hand. This generates a feed instead.

What it is NOT: a substitute for real data. It has no stub quotes, no crosses,
no halts, no 4-million-message auction-imbalance stream, and its price
distribution is invented. Every number in docs/phase9-results.md comes from the
real file and none should ever come from this. What it IS: a stream of
well-formed, self-consistent messages -- every execute, cancel, replace and
delete names a reference the feed already added -- which is exactly what is
needed to prove that a receiver frames, sequences and applies without losing
anything. Conservation is checkable; realism is not claimed.

Deterministic from --seed, so the same arguments give byte-identical output and
a regression gate can hash it.

Usage:
  make-synthetic-feed.py out.gz [--messages N] [--symbols N] [--seed N]
"""
import argparse
import gzip
import random
import struct
import sys

def be(n, width):
    return n.to_bytes(width, "big")

class Feed:
    def __init__(self, symbols, seed):
        self.rng = random.Random(seed)
        self.msgs = []
        self.ts = 34200 * 1_000_000_000      # 09:30:00 in nanoseconds
        self.next_ref = 1
        self.next_match = 1
        # ref -> (locate, shares) for the orders currently resting. The point of
        # keeping it is that nothing this file emits can reference an order that
        # was never added or has already gone -- "0 unknown references" has to
        # be a property of the feed before it can be a result about the book.
        # ref -> (locate, shares, index into live_refs), plus the parallel
        # list, so picking a random resting order is O(1).
        #
        # This was `self.live = {}` with `rng.choice(list(self.live))` to pick,
        # which rebuilds a list of every resting order on every mutating
        # message. At 250,000 messages over 64 symbols the live set stays small
        # and it is imperceptible; at 3,000,000 over 512 symbols the set runs to
        # hundreds of thousands and generation goes quadratic -- five minutes in
        # and a third of the way through a feed that takes the C++ tools two
        # seconds to read. A generator that CI runs cannot be quadratic in the
        # feed it generates.
        self.live = {}
        self.live_refs = []
        # A base price per symbol, and quotes near it. Uniform prices across
        # $10-$1000 would be simpler and would defeat the point: the book keeps
        # a dense array over a band around the touch and a cold std::map for
        # everything outside it, so a feed with scattered prices sends 99% of
        # its adds to the map and any latency measured on it describes
        # std::map::find. Real quotes cluster within pennies of the touch. This
        # random-walks a base and places orders within a few hundred ticks of
        # it, which is not the real distribution but is the right SHAPE -- and
        # shape is what decides which data structure gets exercised.
        self.symbols = [(i + 1, f"SYN{i:05d}"[:8]) for i in range(symbols)]
        self.base = {i + 1: self.rng.randrange(1000, 100000) * 100
                     for i in range(symbols)}

    def head(self, t, locate, length):
        # Placeholder; every timestamp is rewritten by stamp() once the message
        # count is known. See stamp() for why.
        self.ts += 1
        b = bytearray(length)
        b[0] = ord(t)
        b[1:3] = be(locate, 2)
        b[3:5] = be(0, 2)
        b[5:11] = be(self.ts & ((1 << 48) - 1), 6)
        return b

    def emit(self, b):
        self.msgs.append(bytes(b))

    def system_event(self, code):
        b = self.head("S", 0, 12)
        b[11] = ord(code)
        self.emit(b)

    def directory(self, locate, name):
        b = self.head("R", locate, 39)
        b[11:19] = name.ljust(8).encode()
        b[19] = ord("Q")        # market category
        b[20] = ord("N")        # financial status
        b[21:25] = be(100, 4)   # round lot
        b[25] = ord("N")
        self.emit(b)

    def quote(self, locate):
        # The base drifts, so the band has to re-centre occasionally rather than
        # being set once at the open and never tested.
        self.base[locate] += self.rng.randrange(-2, 3) * 100
        if self.base[locate] < 10000:
            self.base[locate] = 10000
        offset = self.rng.randrange(-200, 201) * 100
        return max(100, self.base[locate] + offset)

    def add(self, locate, name):
        ref = self.next_ref
        self.next_ref += 1
        shares = self.rng.randrange(1, 20) * 100
        price = self.quote(locate)
        b = self.head("A", locate, 36)
        b[11:19] = be(ref, 8)
        b[19] = ord(self.rng.choice("BS"))
        b[20:24] = be(shares, 4)
        b[24:32] = name.ljust(8).encode()
        b[32:36] = be(price, 4)
        self.emit(b)
        self.add_live(ref, locate, shares)

    # Swap-remove: the departing ref's slot takes the last entry, and that
    # entry's recorded index follows it. Constant time, and the order of
    # live_refs is not meaningful to anything.
    def add_live(self, ref, locate, shares):
        self.live[ref] = (locate, shares, len(self.live_refs))
        self.live_refs.append(ref)

    def drop_live(self, ref):
        _, _, pos = self.live[ref]
        last = self.live_refs[-1]
        self.live_refs[pos] = last
        lo, sh, _ = self.live[last]
        self.live[last] = (lo, sh, pos)
        self.live_refs.pop()
        del self.live[ref]

    def set_shares(self, ref, shares):
        locate, _, pos = self.live[ref]
        self.live[ref] = (locate, shares, pos)

    def execute(self, ref):
        locate, shares, _ = self.live[ref]
        n = self.rng.randrange(1, shares // 100 + 1) * 100
        b = self.head("E", locate, 31)
        b[11:19] = be(ref, 8)
        b[19:23] = be(n, 4)
        b[23:31] = be(self.next_match, 8)
        self.next_match += 1
        self.emit(b)
        if n >= shares:
            self.drop_live(ref)
        else:
            self.set_shares(ref, shares - n)

    def cancel(self, ref):
        locate, shares, _ = self.live[ref]
        n = self.rng.randrange(1, shares // 100 + 1) * 100
        b = self.head("X", locate, 23)
        b[11:19] = be(ref, 8)
        b[19:23] = be(n, 4)
        self.emit(b)
        if n >= shares:
            self.drop_live(ref)
        else:
            self.set_shares(ref, shares - n)

    def delete(self, ref):
        locate, _, _ = self.live[ref]
        b = self.head("D", locate, 19)
        b[11:19] = be(ref, 8)
        self.emit(b)
        self.drop_live(ref)

    def replace(self, ref):
        locate, _, _ = self.live[ref]
        new_ref = self.next_ref
        self.next_ref += 1
        shares = self.rng.randrange(1, 20) * 100
        price = self.quote(locate)
        b = self.head("U", locate, 35)
        b[11:19] = be(ref, 8)
        b[19:27] = be(new_ref, 8)
        b[27:31] = be(shares, 4)
        b[31:35] = be(price, 4)
        self.emit(b)
        self.drop_live(ref)
        self.add_live(new_ref, locate, shares)

    def build(self, total):
        self.system_event("O")
        for locate, name in self.symbols:
            self.directory(locate, name)
        self.system_event("Q")
        # Warm up so the mutating messages have something to point at.
        for locate, name in self.symbols:
            for _ in range(4):
                self.add(locate, name)
        while len(self.msgs) < total:
            # Weighted toward adds so the resting set does not drain; the real
            # file's mix is roughly half adds, and this is in that neighbourhood
            # without pretending to reproduce it.
            if not self.live_refs or self.rng.random() < 0.5:
                locate, name = self.rng.choice(self.symbols)
                self.add(locate, name)
                continue
            ref = self.live_refs[self.rng.randrange(len(self.live_refs))]
            r = self.rng.random()
            if r < 0.35:
                self.delete(ref)
            elif r < 0.65:
                self.cancel(ref)
            elif r < 0.90:
                self.execute(ref)
            else:
                self.replace(ref)
        # Close the book out. Leaving orders resting is legal on a real day and
        # useless here: an empty book at the end is an invariant a checker can
        # fail on, and a feed that cannot fail its own checker proves nothing.
        while self.live_refs:
            self.delete(self.live_refs[-1])
        self.system_event("M")
        self.system_event("E")
        self.system_event("C")

    def stamp(self, session_seconds):
        """Rewrite every timestamp so the feed spans a realistic session.

        Phase 10.7's rate ladder starts at one times real time, computed by
        itch_census from the feed's own clock. A generator that advanced the
        clock by a uniform 1-2.5 us per message produced a 252,000-message feed
        spanning 0.63 seconds, so "real time" came out at 399,000 msg/s -- above
        the rate at which this pipeline starts dropping, which would have made
        the first rung of the ladder the cliff.

        Arrivals are exponential rather than uniform, which is both closer to
        the truth and the point: a market's messages arrive in bursts, and a
        pipeline sized for the mean of a bursty process is a pipeline that drops
        packets at the peaks. A uniform feed would hide exactly the queueing
        this phase exists to measure.
        """
        n = len(self.msgs)
        if n == 0 or session_seconds <= 0:
            return
        mean_gap = session_seconds * 1e9 / n
        t = 34200 * 1_000_000_000            # 09:30:00
        for i, m in enumerate(self.msgs):
            t += max(1, int(self.rng.expovariate(1.0) * mean_gap))
            b = bytearray(m)
            b[5:11] = be(t & ((1 << 48) - 1), 6)
            self.msgs[i] = bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--messages", type=int, default=200000)
    ap.add_argument("--symbols", type=int, default=64)
    ap.add_argument("--seed", type=int, default=20191230)
    ap.add_argument("--session-seconds", type=float, default=60.0,
                    help="wall time the feed's own timestamps should span; "
                         "sets what one times real time means")
    a = ap.parse_args()

    f = Feed(a.symbols, a.seed)
    f.build(a.messages)
    f.stamp(a.session_seconds)

    # [len][payload], the same framing the rest of the repo reads. mtime=0 so
    # the gzip bytes are reproducible and a hash of the file means something.
    with gzip.GzipFile(a.out, "wb", compresslevel=6, mtime=0) as g:
        for m in f.msgs:
            g.write(struct.pack(">H", len(m)))
            g.write(m)
    rate = len(f.msgs) / a.session_seconds if a.session_seconds > 0 else 0
    print(f"{len(f.msgs)} messages, {a.symbols} symbols, {a.session_seconds:g}s session "
          f"({rate:,.0f} msg/s at 1x) -> {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
