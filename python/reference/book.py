"""Slow, correct order book — the Phase 2 oracle.

Dicts everywhere, no cleverness. `{price: [orders]}` for each side and
`{ref: order}` for lookup. This is the reference the C++ book must match
bit-for-bit in Phase 3.

Only a skeleton for now; fill in the seven mutating handlers in Phase 2.
"""
from collections import defaultdict


class Book:
    def __init__(self):
        self.orders = {}                       # ref -> dict(side, price, shares)
        self.bids = defaultdict(list)          # price -> [refs], FIFO
        self.asks = defaultdict(list)          # price -> [refs], FIFO
        self.volume = 0                        # total executed shares

    def _side(self, side):
        return self.bids if side == "B" else self.asks

    def add(self, ref, side, price, shares):
        self.orders[ref] = {"side": side, "price": price, "shares": shares}
        self._side(side)[price].append(ref)

    def execute(self, ref, shares):
        o = self.orders.get(ref)
        if o is None:
            return
        o["shares"] -= shares
        self.volume += shares
        if o["shares"] <= 0:
            self.delete(ref)

    def cancel(self, ref, shares):
        o = self.orders.get(ref)
        if o is None:
            return
        o["shares"] -= shares
        if o["shares"] <= 0:
            self.delete(ref)

    def delete(self, ref):
        o = self.orders.pop(ref, None)
        if o is None:
            return
        level = self._side(o["side"])[o["price"]]
        level.remove(ref)
        if not level:
            del self._side(o["side"])[o["price"]]

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None
