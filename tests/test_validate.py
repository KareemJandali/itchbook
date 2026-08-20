#!/usr/bin/env python3
"""Tests for the Databento validator's offline half.

The network fetch cannot be tested here, so everything around it is: the fixed
point conversion, the field-by-field comparison, and the exit code. Those are
where a silent bug would turn a real disagreement into a false PASS, which is
the one failure mode that would make the whole validation worthless.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

from analysis import validate   # noqa: E402


class TestPriceConversion(unittest.TestCase):
    def test_dbn_fixed_point_to_price4(self):
        # Databento quotes 1e-9 fixed point; ITCH Price(4) is 1e-4.
        # $10.0000 is 10_000_000_000 there and 100_000 here.
        self.assertEqual(validate.to_itch_price(10_000_000_000), 100_000)
        self.assertEqual(validate.to_itch_price(123_456_700_000), 1_234_567)

    def test_sub_tick_price_is_refused_not_rounded(self):
        # A price finer than Price(4) means the oracle is quoting something we
        # cannot represent. Rounding it would manufacture a false match.
        self.assertIsNone(validate.to_itch_price(10_000_000_001))
        self.assertIsNone(validate.to_itch_price(1))

    def test_none_passes_through(self):
        self.assertIsNone(validate.to_itch_price(None))

    def test_zero_is_a_valid_price(self):
        self.assertEqual(validate.to_itch_price(0), 0)


class TestDateRange(unittest.TestCase):
    def test_end_is_the_following_day(self):
        # Databento ranges are half-open, so a one-day query needs end = start+1
        # or it returns nothing at all.
        self.assertEqual(validate.next_day("2019-01-30"), "2019-01-31")

    def test_crosses_month_and_year_boundaries(self):
        self.assertEqual(validate.next_day("2019-01-31"), "2019-02-01")
        self.assertEqual(validate.next_day("2019-12-31"), "2020-01-01")
        self.assertEqual(validate.next_day("2020-02-28"), "2020-02-29")  # leap year


class TestSessionWindow(unittest.TestCase):
    """ITCH timestamps are ns since ET midnight; vendors bucket daily bars by
    UTC day. Getting this boundary wrong is what made the first real validation
    look like a parser bug when the parser was right."""

    def test_winter_cuts_at_1900_et(self):
        self.assertEqual(validate.utc_day_end_ns("2019-12-30"), 19 * 3600 * 10**9)

    def test_summer_cuts_at_2000_et(self):
        # EDT is UTC-4, so the rollover is an hour later in local terms.
        self.assertEqual(validate.utc_day_end_ns("2019-07-30"), 20 * 3600 * 10**9)

    def test_matches_the_replay_driver(self):
        # Two copies of this rule exist; they must not drift apart.
        from reference import replay
        for date in ("2019-12-30", "2019-07-30", "2020-03-08", "2020-11-01"):
            self.assertEqual(validate.utc_day_end_ns(date),
                             replay.utc_day_end_ns(date), date)

    def test_window_mismatch_is_refused(self):
        ours = {"session_end_ns": None}
        self.assertFalse(validate.check_window(ours, "2019-12-30", "x.json"))

    def test_matching_window_is_accepted(self):
        ours = {"session_end_ns": 19 * 3600 * 10**9}
        self.assertTrue(validate.check_window(ours, "2019-12-30", "x.json"))


class TestCompare(unittest.TestCase):
    def setUp(self):
        self.ours = {"volume": 1000, "open": 100_0000, "high": 101_0000,
                     "low": 99_0000, "close": 100_5000}

    def test_identical_summaries_pass(self):
        rows, ok = validate.compare(self.ours, dict(self.ours))
        self.assertTrue(ok)
        self.assertTrue(all(r[4] == "match" for r in rows))

    def test_a_single_share_of_difference_fails(self):
        theirs = dict(self.ours)
        theirs["volume"] = 1001
        rows, ok = validate.compare(self.ours, theirs)
        self.assertFalse(ok)
        volume_row = next(r for r in rows if r[0].startswith("volume"))
        self.assertEqual(volume_row[3], -1)
        self.assertEqual(volume_row[4], "MISMATCH")

    def test_price_difference_fails(self):
        theirs = dict(self.ours)
        theirs["close"] = 100_5001
        _, ok = validate.compare(self.ours, theirs)
        self.assertFalse(ok)

    def test_missing_on_our_side_fails(self):
        ours = dict(self.ours)
        ours["open"] = None
        rows, ok = validate.compare(ours, self.ours)
        self.assertFalse(ok)
        self.assertIn("MISSING", [r[4] for r in rows])

    def test_absent_oracle_field_is_reported_not_passed(self):
        theirs = dict(self.ours)
        del theirs["high"]
        rows, ok = validate.compare(self.ours, theirs)
        # Nothing to compare against is not a failure, but it must not be
        # silently counted as agreement either.
        self.assertTrue(ok)
        self.assertIn("NO DATA", [r[4] for r in rows])


class TestEndToEndOffline(unittest.TestCase):
    """The whole path except the network, driven through main()."""

    def _run(self, ours, theirs):
        with tempfile.TemporaryDirectory() as d:
            summary = Path(d) / "ours.json"
            oracle = Path(d) / "theirs.json"
            summary.write_text(json.dumps(ours))
            oracle.write_text(json.dumps(theirs))
            return validate.main([str(summary), "--symbol", "TEST",
                                  "--date", "2019-01-30",
                                  "--oracle-json", str(oracle)])

    # 2019-01-30 is EST, so the UTC day rolls over at 19:00 ET.
    WINDOW = {"session_end_ns": 19 * 3600 * 10**9}

    def test_match_exits_zero(self):
        s = {"volume": 100, "open": 1, "high": 2, "low": 3, "close": 4, **self.WINDOW}
        self.assertEqual(self._run(s, dict(s)), 0)

    def test_mismatch_exits_one(self):
        s = {"volume": 100, "open": 1, "high": 2, "low": 3, "close": 4, **self.WINDOW}
        t = dict(s)
        t["volume"] = 99
        self.assertEqual(self._run(s, t), 1)

    def test_unbounded_reconstruction_is_refused(self):
        # No window recorded means the run went to the end of the file, which
        # is a different question from the one the oracle answers.
        s = {"volume": 100, "open": 1, "high": 2, "low": 3, "close": 4}
        self.assertEqual(self._run(s, dict(s)), 1)


class TestReplaySummaryIsComparable(unittest.TestCase):
    """The validator consumes replay.py's --json, so the keys must line up."""

    def test_replay_summary_has_every_field_the_validator_reads(self):
        import gzip
        import make_sample as gen
        from reference import replay

        with tempfile.TemporaryDirectory() as d:
            feed = Path(d) / "s.gz"
            out = Path(d) / "s.json"
            with gzip.open(feed, "wb") as f:
                f.write(gen.build())
            replay.main([str(feed), "--symbol", "TEST", "--json", str(out), "--quiet"])
            summary = json.loads(out.read_text())

        for key, _, _ in validate.FIELDS:
            self.assertIn(key, summary, f"replay.py --json is missing {key!r}")
        # Prices must be integers in Price(4) units, not floats: the comparison
        # against the oracle is exact and floats would make it approximate.
        for key in ("open", "high", "low", "close", "volume"):
            self.assertIsInstance(summary[key], int)


if __name__ == "__main__":
    unittest.main()
