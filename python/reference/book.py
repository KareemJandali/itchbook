"""Slow, correct order book — the Phase 2 oracle.

Dicts everywhere, no cleverness. `{price: [refs]}` for each side and
`{ref: order}` for lookup. This is the reference the C++ book must match
bit-for-bit in Phase 3, so every rule here is written to be read, not to run
fast. When the two disagree, this file is presumed right until proven wrong.

Book mutation rules (ITCH 5.0):

  A / F  add       insert at the *back* of its price level's FIFO
  E      execute   reduce shares at the order's own price; remove at zero
  C      execute   same, but at a stated price; only counts toward volume and
                   the daily price stats when the printable flag is 'Y'
  X      cancel    partial cancel — reduce shares by the stated amount
  D      delete    full removal
  U      replace   delete the old ref, add the new one. Side and stock are
                   inherited; the order goes to the *back* of the new level,
                   because a replace loses queue priority.

  P / Q  trade     real volume that never appears in the displayed book. If your
                   reconstructed daily volume is short by a few percent and the
                   book itself looks right, this is why.
"""
from collections import defaultdict


class Book:
    """One symbol's book, plus the daily trade statistics for that symbol."""

    def __init__(self):
        self.orders = {}                       # ref -> dict(side, price, shares)
        self.bids = defaultdict(list)          # price -> [refs], FIFO order
        self.asks = defaultdict(list)          # price -> [refs], FIFO order

        # ---- daily trade statistics (printable executions only) ----
        self.volume = 0                        # total executed shares
        self.notional = 0                      # sum(price * shares), Price(4) units
        self.trades = 0                        # printable execution count
        self.open = None                       # first printable trade price
        self.close = None                      # last printable trade price
        self.high = None
        self.low = None
        self.hidden_volume = 0                 # 'P' — executions against hidden liquidity
        self.cross_volume = 0                  # 'Q' — opening/closing/halt crosses
        self.cross_prices = {}                 # cross type -> price ('O', 'C', ...)

        # ---- session state ----
        self.trading_state = None              # from 'H': 'H' halted, 'T' trading, ...
        self.system_event = None               # from 'S': last event code seen

        # ---- counters, for spotting a broken feed ----
        self.applied = 0                       # messages consumed by apply()
        self.unknown_ref = 0                   # mutations naming an order we never saw

    # ---- internals -----------------------------------------------------------

    def _side(self, side):
        return self.bids if side == "B" else self.asks

    def _record_trade(self, price, shares):
        """Fold one printable execution into the daily stats.

        A print of zero shares is not a print. An auction that did not happen
        still arrives, as a Cross Trade with shares == 0 and price == 0: every
        symbol that is not NASDAQ-listed gets one at the open, and any symbol
        without a closing auction gets one at the close. Folded in, it sets
        `open` and `close` to zero, drags `low` to zero, and counts a trade
        that never occurred. Both implementations did this, so the differential
        agreed with itself and was wrong together.
        """
        if shares == 0:
            return
        self.volume += shares
        self.notional += price * shares
        self.trades += 1
        if self.open is None:
            self.open = price
        self.close = price
        self.high = price if self.high is None else max(self.high, price)
        self.low = price if self.low is None else min(self.low, price)

    # ---- the seven mutating handlers ----------------------------------------

    def add(self, ref, side, price, shares):
        """'A' / 'F' — new displayed order at the back of its level."""
        self.orders[ref] = {"side": side, "price": price, "shares": shares}
        self._side(side)[price].append(ref)

    def execute(self, ref, shares, price=None, printable=True):
        """'E' / 'C' — shares trade against a resting order.

        `price` is None for 'E' (the trade happens at the order's own price) and
        stated for 'C'. A non-printable 'C' still removes the shares from the
        book but must not count toward volume, VWAP or OHLC.
        """
        o = self.orders.get(ref)
        if o is None:
            self.unknown_ref += 1
            return
        if printable:
            self._record_trade(o["price"] if price is None else price, shares)
        o["shares"] -= shares
        if o["shares"] <= 0:
            self.delete(ref)

    def cancel(self, ref, shares):
        """'X' — partial cancel. A full cancel arrives as 'D', but a cancel that
        happens to take the order to zero removes it just the same."""
        o = self.orders.get(ref)
        if o is None:
            self.unknown_ref += 1
            return
        o["shares"] -= shares
        if o["shares"] <= 0:
            self.delete(ref)

    def delete(self, ref):
        """'D' — full removal."""
        o = self.orders.pop(ref, None)
        if o is None:
            self.unknown_ref += 1
            return
        side = self._side(o["side"])
        level = side[o["price"]]
        level.remove(ref)
        if not level:
            del side[o["price"]]

    def replace(self, orig_ref, new_ref, price, shares):
        """'U' — delete then add. Side and stock come from the original order,
        and the new order goes to the back of its level: a replace loses queue
        priority. That loss is the whole reason phase 6 exists."""
        o = self.orders.get(orig_ref)
        if o is None:
            self.unknown_ref += 1
            return
        side = o["side"]
        self.delete(orig_ref)
        self.add(new_ref, side, price, shares)

    # ---- executions that never touch the book --------------------------------

    def trade(self, price, shares):
        """'P' — non-cross trade against hidden liquidity. Volume only."""
        self.hidden_volume += shares
        self._record_trade(price, shares)

    def cross(self, price, shares, cross_type):
        """'Q' — opening / closing / halt cross. Volume only."""
        self.cross_volume += shares
        # An absent auction is not an auction that printed at zero, and
        # check_cross.py grades this number.
        if shares > 0:
            self.cross_prices[cross_type] = price
        self._record_trade(price, shares)

    # ---- queries -------------------------------------------------------------

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None

    def shares_at(self, side, price):
        """Total displayed shares resting at one price.

        `.get` and not `[...]`: the sides are defaultdicts, so indexing a price
        that isn't there would insert an empty level and make it show up in
        `top()` and `best_bid()`.
        """
        return sum(self.orders[r]["shares"] for r in self._side(side).get(price, ()))

    def top(self, side, levels=10):
        """Top `levels` price levels for one side, best first.

        Returns [(price, shares, order_count), ...] — shorter than `levels` if
        the book is thin.
        """
        book = self._side(side)
        prices = sorted(book, reverse=(side == "B"))[:levels]
        return [(p, self.shares_at(side, p), len(book[p])) for p in prices]

    def vwap(self):
        """Volume-weighted average price, in Price(4) units. None before the
        first printable trade."""
        if self.volume == 0:
            return None
        return self.notional / self.volume

    def crossed(self):
        """True if best bid >= best ask — never legal in a correct book."""
        bid, ask = self.best_bid(), self.best_ask()
        return bid is not None and ask is not None and bid >= ask

    def total_shares(self):
        """Displayed shares resting on both sides."""
        return sum(o["shares"] for o in self.orders.values())

    # ---- message application -------------------------------------------------

    def apply(self, m):
        """Apply one decoded message (from `parser.decode`) to the book.

        Messages for other symbols must be filtered out before they get here —
        this book models one symbol.
        """
        t = m["type"]
        if t in ("A", "F", "E", "C", "X", "D", "U", "P", "Q"):
            self.applied += 1
        if t in ("A", "F"):
            self.add(m["ref"], m["side"], m["price"], m["shares"])
        elif t == "E":
            self.execute(m["ref"], m["shares"])
        elif t == "C":
            self.execute(m["ref"], m["shares"], price=m["price"],
                         printable=m["printable"])
        elif t == "X":
            self.cancel(m["ref"], m["shares"])
        elif t == "D":
            self.delete(m["ref"])
        elif t == "U":
            self.replace(m["orig_ref"], m["ref"], m["price"], m["shares"])
        elif t == "P":
            self.trade(m["price"], m["shares"])
        elif t == "Q":
            self.cross(m["price"], m["shares"], m["cross_type"])
        elif t == "H":
            self.trading_state = m["state"]
        elif t == "S":
            self.system_event = m["code"]
        # 'R' and anything else: metadata, no book effect.
