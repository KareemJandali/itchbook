"""Slow, obvious queue-position simulator — the Phase 6 oracle.

Mirrors include/itchbook/sim/queue_model.hpp decision for decision. The two must
produce byte-identical fill logs; where they disagree, one is wrong and it is
usually not this one. That discipline caught real bugs in phases 2 and 3.

The order of operations per message is load-bearing and matches the C++ driver:

  1. session/halt state first, so all lanes see identical eligibility
  2. pre-mutation snapshot (E/C/X/D/U carry only an order reference)
  3. mutate the book
  4. resolve and commit to every lane
  5. price priority against the post-mutation book
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from reference import parser as itch          # noqa: E402
from reference.book import Book               # noqa: E402

NAIVE, OPTIMISTIC, MBO, PESSIMISTIC = "naive", "optimistic", "mbo", "pessimistic"
MODELS = (NAIVE, OPTIMISTIC, MBO, PESSIMISTIC)

TRADE, CANCEL, ADD, NONE = "trade", "cancel", "add", "none"


def at_or_through(side, limit, px):
    return px <= limit if side == "B" else px >= limit


def strictly_through(side, limit, px):
    return px < limit if side == "B" else px > limit


def shares_at(book, side, price):
    level = book.bids if side == "B" else book.asks
    return sum(book.orders[r]["shares"] for r in level.get(price, ()))


def refs_at(book, side, price):
    level = book.bids if side == "B" else book.asks
    return list(level.get(price, ()))


def resolve(m, pre):
    """A decoded message plus the pre-mutation state becomes a queue event."""
    r = {"type": m["type"], "cls": NONE, "known": False, "ref": 0,
         "side": "B", "resting_price": 0, "shares": 0,
         "printable": False, "print_price": 0, "ts": m["ts"]}
    t = m["type"]

    if t in ("A", "F"):
        r.update(cls=ADD, side=m["side"], resting_price=m["price"], shares=m["shares"])
    elif t == "E":
        if pre is None:
            return r
        r.update(cls=TRADE, known=True, ref=m["ref"], side=pre["side"],
                 resting_price=pre["price"], shares=m["shares"],
                 printable=True, print_price=pre["price"])
    elif t == "C":
        if pre is None:
            return r
        # THE rule: a C fills only when printable AND printing at the resting
        # price. A non-printable C is already reported elsewhere as part of an
        # aggregate cross print, and a repriced C matched under a regime this
        # model does not describe.
        cls = TRADE if (m["printable"] and m["price"] == pre["price"]) else CANCEL
        r.update(cls=cls, known=True, ref=m["ref"], side=pre["side"],
                 resting_price=pre["price"], shares=m["shares"],
                 printable=m["printable"], print_price=m["price"])
    elif t == "X":
        if pre is None:
            return r
        r.update(cls=CANCEL, known=True, ref=m["ref"], side=pre["side"],
                 resting_price=pre["price"], shares=m["shares"])
    elif t in ("D", "U"):
        # A delete removes whatever was left, which only the pre-mutation book
        # knows. A U is delete-then-add; the add half joins the back and has no
        # effect on anyone already resting.
        if pre is None:
            return r
        ref = m["ref"] if t == "D" else m["orig_ref"]
        r.update(cls=CANCEL, known=True, ref=ref, side=pre["side"],
                 resting_price=pre["price"], shares=pre["shares"])
    elif t == "P":
        # Hidden liquidity never sat in the displayed FIFO, so known stays False
        # and it cannot advance a queue. Its side field is deliberately unused.
        r.update(cls=TRADE, known=False, shares=m["shares"], printable=True,
                 print_price=m["price"], resting_price=m["price"])
    elif t == "Q":
        r.update(cls=NONE, shares=m["shares"], printable=True,
                 print_price=m["price"], resting_price=m["price"])
    return r


class Entry:
    def __init__(self, oid, side, price, quantity, display, arrived_ns):
        self.id = oid
        self.side = side
        self.price = price
        self.display = display if 0 < display < quantity else quantity
        self.slice_size = self.display
        self.hidden = quantity - self.display
        self.ahead = 0
        self.ahead0 = 0
        self.arrived_ns = arrived_ns
        self.refreshes = 0
        self.live = True


class QueueModel:
    def __init__(self, model, clamp=True, naive_fills_hidden=True):
        self.model = model
        self.clamp = clamp
        self.naive_fills_hidden = naive_fills_hidden
        self.entries = []
        self.ahead_sets = {}
        self.fills = []
        self.clamp_events = 0
        self.priority_anomalies = 0
        self.tradable = False

    # ---- our orders ----

    def place(self, book, oid, side, price, quantity, display, arrival_ns):
        e = Entry(oid, side, price, quantity, display, arrival_ns)
        e.ahead = shares_at(book, side, price) + self._own_ahead(side, price, arrival_ns)
        e.ahead0 = e.ahead
        self.entries.append(e)
        if self.model == MBO:
            self.ahead_sets[oid] = {
                r: book.orders[r]["shares"] for r in refs_at(book, side, price)
            }

    def _own_ahead(self, side, price, arrived_ns):
        return sum(o.display for o in self.entries
                   if o.live and o.display > 0 and o.side == side
                   and o.price == price and o.arrived_ns < arrived_ns)

    # ---- arithmetic ----

    def _trade_class(self, e, removed, ts, trigger, book):
        used = min(removed, e.ahead)
        e.ahead -= used
        overflow = removed - used
        if overflow == 0:
            return
        fill = min(overflow, e.display)     # capped at the DISPLAYED slice
        if fill == 0:
            return
        e.display -= fill
        e.ahead = 0
        self.fills.append({"model": self.model, "order_id": e.id, "ts": ts,
                           "side": e.side, "price": e.price, "shares": fill,
                           "trigger": trigger})
        if e.display == 0 and e.hidden > 0:
            self._refresh(e, book)

    @staticmethod
    def _cancel_class(e, removed, from_ahead):
        if from_ahead:
            e.ahead -= min(removed, e.ahead)

    def _refresh(self, e, book):
        slice_size = min(e.hidden, e.slice_size)
        e.hidden -= slice_size
        e.display = slice_size
        e.refreshes += 1
        e.ahead = (shares_at(book, e.side, e.price)
                   + self._own_ahead(e.side, e.price, e.arrived_ns))

    # ---- commit ----

    def commit(self, r, book):
        if not self.tradable:
            return
        for e in self.entries:
            if not e.live or e.display == 0:
                continue
            if self.model == NAIVE:
                self._naive(e, r, book)
            else:
                self._queued(e, r, book)
        if self.clamp:
            self._clamp(book)
        self._reap()

    def _opposite_reaches_us(self, e, book):
        best = book.best_ask() if e.side == "B" else book.best_bid()
        return best is not None and at_or_through(e.side, e.price, best)

    def _queued(self, e, r, book):
        # R3: a printable trade strictly through our limit. Side is checked only
        # when the message carries a reliable one, and it stands down while R1 is
        # live so the two mechanisms cannot claim the same flow.
        if (r["cls"] == TRADE and r["printable"]
                and strictly_through(e.side, e.price, r["print_price"])
                and (not r["known"] or r["side"] == e.side)
                and not self._opposite_reaches_us(e, book)):
            self._trade_class(e, r["shares"], r["ts"], "through", book)
            return
        if not r["known"] or r["side"] != e.side or r["resting_price"] != e.price:
            return
        if r["cls"] == TRADE:
            self._trade_class(e, r["shares"], r["ts"], "execution", book)
            if self.model == MBO:
                self._note_trade(e, r)
        elif r["cls"] == CANCEL:
            self._cancel_class(e, r["shares"], self._cancel_is_ahead(e, r))

    def _cancel_is_ahead(self, e, r):
        if self.model == OPTIMISTIC:
            return True
        if self.model == PESSIMISTIC:
            return False
        return r["ref"] in self.ahead_sets.get(e.id, {})

    def _note_trade(self, e, r):
        s = self.ahead_sets.get(e.id)
        if s is None:
            return
        if r["ref"] not in s:
            if e.ahead > 0:
                self.priority_anomalies += 1
            return
        if r["shares"] >= s[r["ref"]]:
            del s[r["ref"]]
        else:
            s[r["ref"]] -= r["shares"]

    def _naive(self, e, r, book):
        if r["cls"] != TRADE or not r["printable"]:
            return
        if r["type"] == "Q":
            return
        if r["type"] == "P" and not self.naive_fills_hidden:
            return
        if not at_or_through(e.side, e.price, r["print_price"]):
            return
        fill = min(r["shares"], e.display)
        if fill == 0:
            return
        e.display -= fill
        self.fills.append({"model": self.model, "order_id": e.id, "ts": r["ts"],
                           "side": e.side, "price": e.price, "shares": fill,
                           "trigger": "naive"})
        if e.display == 0 and e.hidden > 0:
            self._refresh(e, book)

    def apply_price_priority(self, book, ts):
        """R1: the other side came to our price. Identical in all four models."""
        if not self.tradable:
            return
        for e in self.entries:
            if not e.live or e.display == 0:
                continue
            best = book.best_ask() if e.side == "B" else book.best_bid()
            if best is None or not at_or_through(e.side, e.price, best):
                continue
            other = "S" if e.side == "B" else "B"
            reach = 0
            for price, _shares, _count in book.top(other, 32):
                if not at_or_through(e.side, e.price, price):
                    break
                reach += _shares
            if reach == 0:
                continue
            self._trade_class(e, reach, ts, "lock", book)
        self._reap()

    def _clamp(self, book):
        for e in self.entries:
            if not e.live or e.display == 0:
                continue
            limit = (shares_at(book, e.side, e.price)
                     + self._own_ahead(e.side, e.price, e.arrived_ns))
            if e.ahead > limit:
                self.clamp_events += 1
                e.ahead = limit

    def _reap(self):
        for e in self.entries:
            if e.live and e.display == 0 and e.hidden == 0:
                e.live = False


def run(path, placements):
    """placements: list of (side, price, quantity, after, display)."""
    lanes = {m: QueueModel(m) for m in MODELS}
    book = Book()
    index = 0
    placed = [False] * len(placements)

    opener = __import__("gzip").open if str(path).endswith(".gz") else open
    with opener(path, "rb") as f:
        for mtype, payload in itch.iter_messages(f):
            m = itch.decode(mtype, payload)
            if m is None:
                continue

            # 1. session and halt state
            if m["type"] == "H":
                for q in lanes.values():
                    q.tradable = m["state"] == "T"
            elif m["type"] == "S":
                if m["code"] == "Q":
                    for q in lanes.values():
                        q.tradable = True
                elif m["code"] in ("M", "C"):
                    for q in lanes.values():
                        q.tradable = False

            # 2. pre-mutation snapshot
            pre = None
            ref = m.get("orig_ref") if m["type"] == "U" else m.get("ref")
            if m["type"] in ("E", "C", "X", "D", "U") and ref is not None:
                o = book.orders.get(ref)
                if o is not None:
                    pre = {"side": o["side"], "price": o["price"], "shares": o["shares"]}

            # 3. mutate
            book.apply(m)

            # 4. resolve and commit
            r = resolve(m, pre)
            for q in lanes.values():
                q.commit(r, book)

            # 5. price priority
            for q in lanes.values():
                q.apply_price_priority(book, m["ts"])

            index += 1
            for i, (side, price, qty, after, display) in enumerate(placements):
                if placed[i] or index < after:
                    continue
                for q in lanes.values():
                    q.place(book, 1000000 + i, side, price, qty, display, m["ts"])
                placed[i] = True

    return lanes, index
