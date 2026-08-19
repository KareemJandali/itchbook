#!/usr/bin/env python3
"""Tests for the Phase 2 Python oracle — parser, book, and replay driver.

Stdlib unittest only, so CI needs nothing installed:

    python3 -m unittest discover -s tests -p 'test_*.py' -v

The end-to-end expectations are derived by hand from what `make_sample.py`
emits, not by running the book and recording what it said. That direction
matters: a test written from the implementation's output only proves the code is
consistent with itself.
"""
import gzip
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import make_sample as gen                      # noqa: E402
from reference import parser as itch           # noqa: E402
from reference.book import Book                # noqa: E402
from reference import replay                   # noqa: E402

MID = gen.MID
TICK = gen.TICK


def decode_one(payload):
    """Frame, re-read and decode a single payload — exercises the real path."""
    stream = io.BytesIO(gen.frame(payload))
    (mtype, p), = itch.iter_messages(stream)
    return itch.decode(mtype, p)


class TestFraming(unittest.TestCase):
    def test_length_prefix_must_match_spec(self):
        # A payload whose prefix disagrees with the spec length for its type is
        # a desync, and must fail immediately rather than 200MB later.
        bad = gen.order_delete(gen.T0, 42) + b"\x00"   # 'D' is 19, this is 20
        with self.assertRaises(ValueError):
            list(itch.iter_messages(io.BytesIO(gen.frame(bad))))

    def test_truncated_body_raises(self):
        framed = gen.frame(gen.order_delete(gen.T0, 42))
        with self.assertRaises(ValueError):
            list(itch.iter_messages(io.BytesIO(framed[:-3])))

    def test_truncated_prefix_raises(self):
        with self.assertRaises(ValueError):
            list(itch.iter_messages(io.BytesIO(b"\x00")))

    def test_clean_eof_is_not_an_error(self):
        self.assertEqual(list(itch.iter_messages(io.BytesIO(b""))), [])

    def test_every_modelled_type_has_a_spec_length(self):
        self.assertEqual(set(itch.SPEC_LENGTH), set(itch.DECODERS))

    def test_generated_payloads_match_spec_lengths(self):
        for mtype, payload in itch.iter_messages(io.BytesIO(gen.build())):
            self.assertEqual(len(payload), itch.SPEC_LENGTH[mtype],
                             f"{mtype!r} payload is the wrong length")


class TestHeader(unittest.TestCase):
    def test_timestamp_is_six_bytes_big_endian(self):
        ns = 34_200_123_456_789
        m = decode_one(gen.order_delete(ns, 1))
        self.assertEqual(m["ts"], ns)

    def test_timestamp_survives_the_top_of_the_range(self):
        # 6 bytes holds ~78 hours of nanoseconds; a full day must not overflow.
        ns = 2**48 - 1
        self.assertEqual(decode_one(gen.order_delete(ns, 1))["ts"], ns)

    def test_stock_locate_and_tracking(self):
        payload = gen.order_delete(gen.T0, 1)
        self.assertEqual(itch.stock_locate(payload), gen.LOC)
        self.assertEqual(itch.tracking_number(payload), 0)


class TestDecoders(unittest.TestCase):
    def test_add_order(self):
        m = decode_one(gen.add_order(gen.T0, 7, b"B", 300, MID))
        self.assertEqual((m["type"], m["ref"], m["side"], m["shares"],
                          m["stock"], m["price"]),
                         ("A", 7, "B", 300, "TEST", MID))

    def test_add_order_mpid_is_an_add_plus_attribution(self):
        m = decode_one(gen.add_order_mpid(gen.T0, 7, b"S", 300, MID, "NSDQ"))
        self.assertEqual(m["type"], "F")
        self.assertEqual(m["attribution"], "NSDQ")
        self.assertEqual((m["ref"], m["side"], m["shares"], m["price"]),
                         (7, "S", 300, MID))

    def test_order_executed(self):
        m = decode_one(gen.order_executed(gen.T0, 7, 40, 99))
        self.assertEqual((m["type"], m["ref"], m["shares"], m["match"]),
                         ("E", 7, 40, 99))

    def test_order_executed_with_price_printable_flag(self):
        yes = decode_one(gen.order_executed_price(gen.T0, 7, 40, 99, b"Y", MID))
        no = decode_one(gen.order_executed_price(gen.T0, 7, 40, 99, b"N", MID))
        self.assertTrue(yes["printable"])
        self.assertFalse(no["printable"])
        self.assertEqual(yes["price"], MID)

    def test_order_cancel_and_delete(self):
        x = decode_one(gen.order_cancel(gen.T0, 7, 25))
        d = decode_one(gen.order_delete(gen.T0, 7))
        self.assertEqual((x["ref"], x["shares"]), (7, 25))
        self.assertEqual(d["ref"], 7)

    def test_order_replace_carries_both_refs(self):
        m = decode_one(gen.order_replace(gen.T0, 7, 8, 300, MID))
        self.assertEqual((m["orig_ref"], m["ref"], m["shares"], m["price"]),
                         (7, 8, 300, MID))

    def test_trade_and_cross(self):
        p = decode_one(gen.trade(gen.T0, 7, b"B", 250, MID, 99))
        q = decode_one(gen.cross_trade(gen.T0, 5000, MID, 99, b"O"))
        self.assertEqual((p["shares"], p["price"], p["stock"]), (250, MID, "TEST"))
        self.assertEqual((q["shares"], q["price"], q["cross_type"]), (5000, MID, "O"))

    def test_trading_action_and_system_event(self):
        h = decode_one(gen.trading_action(gen.T0, b"H", "LUDP"))
        s = decode_one(gen.system_event(gen.T0, b"Q"))
        self.assertEqual((h["stock"], h["state"], h["reason"]), ("TEST", "H", "LUDP"))
        self.assertEqual(s["code"], "Q")

    def test_unmodelled_type_decodes_to_none(self):
        # 'Y' (Reg SHO) has no decoder yet; it must be skipped, not crash.
        self.assertIsNone(itch.decode(b"Y", b"Y" + b"\x00" * 19))


class TestBookMechanics(unittest.TestCase):
    def setUp(self):
        self.b = Book()

    def test_fifo_order_within_a_level(self):
        self.b.add(1, "B", MID, 100)
        self.b.add(2, "B", MID, 200)
        self.b.add(3, "B", MID, 300)
        self.assertEqual(self.b.bids[MID], [1, 2, 3])
        self.assertEqual(self.b.shares_at("B", MID), 600)

    def test_delete_preserves_the_order_of_the_survivors(self):
        for ref in (1, 2, 3):
            self.b.add(ref, "B", MID, 100)
        self.b.delete(2)
        self.assertEqual(self.b.bids[MID], [1, 3])

    def test_replace_goes_to_the_back_and_loses_priority(self):
        self.b.add(1, "S", MID, 100)
        self.b.add(2, "S", MID, 100)
        self.b.replace(1, 9, MID, 100)
        # Ref 1 was first in the queue; its replacement sits behind ref 2.
        self.assertEqual(self.b.asks[MID], [2, 9])

    def test_replace_inherits_the_side(self):
        self.b.add(1, "S", MID, 100)
        self.b.replace(1, 9, MID + TICK, 50)
        self.assertEqual(self.b.orders[9]["side"], "S")
        self.assertEqual(self.b.asks[MID + TICK], [9])
        self.assertNotIn(MID, self.b.asks)

    def test_execute_reduces_then_removes_at_zero(self):
        self.b.add(1, "B", MID, 100)
        self.b.execute(1, 40)
        self.assertEqual(self.b.orders[1]["shares"], 60)
        self.b.execute(1, 60)
        self.assertNotIn(1, self.b.orders)
        self.assertNotIn(MID, self.b.bids)      # the empty level goes too
        self.assertEqual(self.b.volume, 100)

    def test_execute_uses_the_orders_own_price(self):
        self.b.add(1, "B", MID, 100)
        self.b.execute(1, 100)
        self.assertEqual(self.b.notional, MID * 100)

    def test_execute_with_price_uses_the_stated_price(self):
        self.b.add(1, "B", MID, 100)
        self.b.execute(1, 100, price=MID - 50)
        self.assertEqual(self.b.notional, (MID - 50) * 100)

    def test_non_printable_execution_moves_the_book_but_not_the_volume(self):
        self.b.add(1, "B", MID, 100)
        self.b.execute(1, 30, price=MID, printable=False)
        self.assertEqual(self.b.orders[1]["shares"], 70)   # book moved
        self.assertEqual(self.b.volume, 0)                 # volume did not
        self.assertEqual(self.b.trades, 0)
        self.assertIsNone(self.b.open)

    def test_cancel_to_zero_removes_the_order(self):
        self.b.add(1, "B", MID, 100)
        self.b.cancel(1, 100)
        self.assertNotIn(1, self.b.orders)
        self.assertEqual(self.b.volume, 0)     # a cancel is not a trade

    def test_mutations_on_an_unknown_ref_are_counted_not_crashed(self):
        for call in (lambda: self.b.execute(42, 10),
                     lambda: self.b.cancel(42, 10),
                     lambda: self.b.delete(42),
                     lambda: self.b.replace(42, 43, MID, 10)):
            call()
        self.assertEqual(self.b.unknown_ref, 4)
        self.assertEqual(self.b.orders, {})

    def test_trades_never_touch_the_book(self):
        self.b.add(1, "B", MID, 100)
        self.b.trade(MID, 250)
        self.b.cross(MID, 5000, "O")
        self.assertEqual(self.b.total_shares(), 100)
        self.assertEqual(self.b.volume, 5250)
        self.assertEqual(self.b.hidden_volume, 250)
        self.assertEqual(self.b.cross_volume, 5000)

    def test_best_bid_ask_and_top_ordering(self):
        for i in range(3):
            self.b.add(100 + i, "B", MID - (i + 1) * TICK, 10)
            self.b.add(200 + i, "S", MID + (i + 1) * TICK, 20)
        self.assertEqual(self.b.best_bid(), MID - TICK)     # highest bid
        self.assertEqual(self.b.best_ask(), MID + TICK)     # lowest ask
        self.assertEqual([p for p, _, _ in self.b.top("B", 3)],
                         [MID - TICK, MID - 2 * TICK, MID - 3 * TICK])
        self.assertEqual([p for p, _, _ in self.b.top("S", 3)],
                         [MID + TICK, MID + 2 * TICK, MID + 3 * TICK])
        self.assertFalse(self.b.crossed())

    def test_top_is_short_when_the_book_is_thin(self):
        self.b.add(1, "B", MID, 10)
        self.assertEqual(len(self.b.top("B", 10)), 1)

    def test_querying_an_absent_price_does_not_create_a_level(self):
        # The sides are defaultdicts — a careless read would insert an empty
        # level and corrupt best_bid()/top().
        self.assertEqual(self.b.shares_at("B", MID), 0)
        self.assertEqual(self.b.bids, {})
        self.assertIsNone(self.b.best_bid())

    def test_vwap_is_none_before_the_first_trade(self):
        self.assertIsNone(self.b.vwap())


class TestSampleReplay(unittest.TestCase):
    """End-to-end over the generated sample, against hand-derived numbers."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.feed = Path(cls.tmp.name) / "sample.gz"
        with gzip.open(cls.feed, "wb") as f:
            f.write(gen.build())
        cls.book, cls.read, _ = replay.replay(cls.feed, symbol="TEST")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    # ---- daily statistics ----
    #
    # Printable executions in the sample, in order:
    #   opening cross  5000 @ MID
    #   'E'              40 @ MID -  100   (the order's own price)
    #   'C' printable    50 @ MID -  150   (the stated price)
    #   'C' non-print    30 @ MID -  250   -> excluded from all of this
    #   'P' hidden      250 @ MID
    #   closing cross  3000 @ MID +  200

    def test_volume_excludes_the_non_printable_execution(self):
        self.assertEqual(self.book.volume, 5000 + 40 + 50 + 250 + 3000)
        self.assertEqual(self.book.trades, 5)

    def test_hidden_and_cross_volume_are_broken_out(self):
        self.assertEqual(self.book.hidden_volume, 250)
        self.assertEqual(self.book.cross_volume, 5000 + 3000)

    def test_ohlc(self):
        self.assertEqual(self.book.open, MID)                # opening cross
        self.assertEqual(self.book.close, MID + 2 * TICK)    # closing cross
        self.assertEqual(self.book.high, MID + 2 * TICK)
        self.assertEqual(self.book.low, MID - 150)           # the printable 'C'

    def test_vwap(self):
        notional = (5000 * MID + 40 * (MID - 100) + 50 * (MID - 150) +
                    250 * MID + 3000 * (MID + 200))
        volume = 5000 + 40 + 50 + 250 + 3000
        self.assertEqual(self.book.notional, notional)
        self.assertAlmostEqual(self.book.vwap(), notional / volume)

    def test_cross_prices_recorded_by_type(self):
        self.assertEqual(self.book.cross_prices, {"O": MID, "C": MID + 2 * TICK})

    # ---- book state ----

    def test_book_is_not_crossed(self):
        self.assertFalse(self.book.crossed())
        self.assertEqual(self.book.best_bid(), MID - TICK)
        self.assertEqual(self.book.best_ask(), MID + TICK)

    def test_best_bid_level_after_add_mpid_and_execution(self):
        # bid ref 1000 rests 100, 'F' ref 3000 joins with 500, 'E' takes 40.
        self.assertEqual(self.book.shares_at("B", MID - TICK), 100 + 500 - 40)
        self.assertEqual(self.book.bids[MID - TICK], [1000, 3000])

    def test_printable_c_leaves_the_order_at_its_own_price(self):
        # The 'C' executed at MID-150, but ref 1001 rests at MID-200.
        self.assertEqual(self.book.orders[1001]["price"], MID - 2 * TICK)
        self.assertEqual(self.book.shares_at("B", MID - 2 * TICK), 101 - 50)

    def test_non_printable_c_still_removed_the_shares(self):
        self.assertEqual(self.book.shares_at("B", MID - 3 * TICK), 102 - 30)

    def test_cancel_delete_and_replace_on_the_ask_side(self):
        self.assertEqual(self.book.shares_at("S", MID + TICK), 200 - 50)  # 'X'
        self.assertNotIn(MID + 2 * TICK, self.book.asks)                  # 'D'
        self.assertNotIn(MID + 3 * TICK, self.book.asks)                  # 'U' left
        # ...and the replacement landed at the back of MID + 5 ticks.
        self.assertEqual(self.book.asks[MID + 5 * TICK], [2004, 4000])
        self.assertEqual(self.book.shares_at("S", MID + 5 * TICK), 204 + 300)

    def test_totals(self):
        bid_shares = sum(100 + i for i in range(gen.LEVELS)) + 500 - 40 - 50 - 30
        ask_shares = (sum(200 + i for i in range(gen.LEVELS))
                      - 50            # 'X' partial cancel
                      - 201           # 'D' deleted ref 2001
                      - 202 + 300)    # 'U' replaced ref 2002 with 300 shares
        self.assertEqual(self.book.total_shares(), bid_shares + ask_shares)
        resting = (2 * gen.LEVELS   # the 'A' adds, both sides
                   + 1              # the 'F' add
                   - 1              # the 'D' delete
                   - 1 + 1)         # the 'U' replace: one out, one in
        self.assertEqual(len(self.book.orders), resting)
        self.assertEqual(self.book.unknown_ref, 0)

    def test_session_state(self):
        self.assertEqual(self.book.system_event, "C")   # end of messages
        self.assertEqual(self.book.trading_state, "T")

    def test_message_count(self):
        self.assertEqual(self.read, 56)
        # Every message except the 4 'S', the 1 'R' and the 1 'H'.
        self.assertEqual(self.book.applied, 50)

    def test_limit_stops_early(self):
        book, read, _ = replay.replay(self.feed, symbol="TEST", limit=5)
        self.assertEqual(read, 5)
        self.assertLess(book.applied, self.book.applied)

    def test_unknown_symbol_is_an_error(self):
        with self.assertRaises(SystemExit):
            replay.replay(self.feed, symbol="NOPE")

    def test_filtering_ignores_other_symbols(self):
        # A stray message on another locate must not reach our book.
        payload = bytearray(gen.add_order(gen.T0, 77, b"B", 999, MID))
        payload[1:3] = (42).to_bytes(2, "big")          # a different locate
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "mixed.gz"
            with gzip.open(path, "wb") as f:
                f.write(gen.build() + gen.frame(bytes(payload)))
            book, _, _ = replay.replay(path, symbol="TEST")
        self.assertNotIn(77, book.orders)
        self.assertEqual(book.total_shares(), self.book.total_shares())


class TestSnapshots(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.feed = Path(self.tmp.name) / "sample.gz"
        with gzip.open(self.feed, "wb") as f:
            f.write(gen.build())

    def tearDown(self):
        self.tmp.cleanup()

    def _run(self, interval_ns, levels):
        out = Path(self.tmp.name) / "book.csv"
        book, _, written = replay.replay(self.feed, symbol="TEST", snapshots=out,
                                         interval_ns=interval_ns, levels=levels)
        rows = out.read_text().splitlines()
        return book, written, rows

    def test_header_and_row_shape(self):
        _, written, rows = self._run(5_000_000, 3)
        self.assertEqual(rows[0].split(",")[:5],
                         ["ts", "ask_px_1", "ask_sz_1", "bid_px_1", "bid_sz_1"])
        self.assertEqual(len(rows[0].split(",")), 1 + 4 * 3)
        self.assertEqual(len(rows) - 1, written)
        for row in rows[1:]:
            self.assertEqual(len(row.split(",")), 1 + 4 * 3)

    def test_rows_land_on_the_interval_grid(self):
        interval = 5_000_000
        _, _, rows = self._run(interval, 2)
        stamps = [int(r.split(",")[0]) for r in rows[1:]]
        self.assertTrue(all(t % interval == 0 for t in stamps))
        self.assertEqual(stamps, sorted(stamps))
        self.assertEqual(len(set(stamps)), len(stamps))
        self.assertTrue(all(t > gen.T0 for t in stamps))

    def test_thin_book_uses_lobster_dummy_fills(self):
        # 30 levels requested, but the sample only has 20 a side.
        _, _, rows = self._run(5_000_000, 30)
        last = rows[-1].split(",")
        self.assertEqual(int(last[1 + 4 * 25 + 0]), replay.NO_ASK_PRICE)
        self.assertEqual(int(last[1 + 4 * 25 + 1]), 0)
        self.assertEqual(int(last[1 + 4 * 25 + 2]), replay.NO_BID_PRICE)
        self.assertEqual(int(last[1 + 4 * 25 + 3]), 0)

    def test_snapshot_prices_are_never_crossed(self):
        _, _, rows = self._run(1_000_000, 1)
        for row in rows[1:]:
            _, ask, _, bid, _ = row.split(",")
            if int(ask) != replay.NO_ASK_PRICE and int(bid) != replay.NO_BID_PRICE:
                self.assertLess(int(bid), int(ask))

    def test_a_finer_grid_produces_at_least_as_many_rows(self):
        _, coarse, _ = self._run(10_000_000, 2)
        _, fine, _ = self._run(1_000_000, 2)
        self.assertGreater(fine, coarse)


if __name__ == "__main__":
    unittest.main()
