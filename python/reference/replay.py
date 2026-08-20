#!/usr/bin/env python3
"""Replay an ITCH 5.0 feed through the reference book — the Phase 2 driver.

Reconstructs one symbol's book message by message, writes periodic top-of-book
snapshots to CSV, and prints the daily summary (volume, OHLC, VWAP) that gets
checked against NASDAQ's published daily figures or a LOBSTER orderbook file.

    python3 python/reference/replay.py data/raw/sample.gz --symbol TEST \
        --snapshots data/sliced/TEST_book.csv --interval-ms 100

The C++ `book_replay` in Phase 3 must emit a byte-identical snapshot CSV for the
same input. Not "close" — identical.

Snapshot timing: snapshots land on a fixed grid of multiples of the interval,
anchored at midnight, starting at the first grid point strictly after the first
message. Each row is the book state as of just *before* the first message with a
timestamp at or past that grid point. The rule is stated this precisely because
the C++ side has to reproduce it exactly.

Column layout follows LOBSTER's orderbook files — ask price, ask size, bid price,
bid size, repeated per level — so a slice of this CSV can be diffed straight
against a LOBSTER file. Missing levels use LOBSTER's dummy fills.
"""
import argparse
import datetime
import gzip
import json
import sys
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from reference import parser as itch
from reference.book import Book

# LOBSTER's dummy fills for levels a thin book doesn't reach.
NO_ASK_PRICE = 9999999999
NO_BID_PRICE = -9999999999


def utc_day_end_ns(date):
    """When the UTC day rolls over, in ns since that trading day's ET midnight.

    ITCH timestamps are nanoseconds since midnight Eastern. Vendors that bucket
    daily bars by UTC day therefore cut the session at 19:00 ET in winter and
    20:00 ET in summer, leaving the tail of after-hours trading in the *next*
    bar. Comparing a full-file reconstruction against such a bar shows up as a
    small volume excess and a wrong close, which looks like a bug and is not.

    Computed from the zone rather than hardcoded, so it stays right across DST.
    """
    et = ZoneInfo("America/New_York")
    d = datetime.date.fromisoformat(date)
    rollover = (datetime.datetime(d.year, d.month, d.day, tzinfo=datetime.timezone.utc)
                + datetime.timedelta(days=1))
    et_midnight = datetime.datetime(d.year, d.month, d.day, tzinfo=et)
    return int((rollover.astimezone(et) - et_midnight).total_seconds() * 1e9)


def open_feed(path):
    """Open a feed, gzipped or not. Real days are .gz; test slices may not be."""
    path = str(path)
    if path.endswith(".gz"):
        return gzip.open(path, "rb")
    return open(path, "rb")


def snapshot_header(levels):
    cols = ["ts"]
    for i in range(1, levels + 1):
        cols += [f"ask_px_{i}", f"ask_sz_{i}", f"bid_px_{i}", f"bid_sz_{i}"]
    return ",".join(cols)


def snapshot_row(book, ts, levels):
    asks = book.top("S", levels)
    bids = book.top("B", levels)
    fields = [str(ts)]
    for i in range(levels):
        if i < len(asks):
            fields += [str(asks[i][0]), str(asks[i][1])]
        else:
            fields += [str(NO_ASK_PRICE), "0"]
        if i < len(bids):
            fields += [str(bids[i][0]), str(bids[i][1])]
        else:
            fields += [str(NO_BID_PRICE), "0"]
    return ",".join(fields)


def px(price):
    """Price(4) integer -> human-readable dollars."""
    return "-" if price is None else f"{price / 10000:.4f}"


def replay(path, symbol=None, snapshots=None, interval_ns=60_000_000_000,
           levels=10, limit=None, end_ns=None):
    """Replay `path`, returning (book, messages_read, snapshots_written).

    With `symbol`, the stock locate code is resolved from the Stock Directory
    ('R') block at the top of the file and every later message is filtered on
    it. Without it, every message is applied — correct only for a single-symbol
    slice produced by `itch_slice`.
    """
    book = Book()
    locate = None            # target stock locate, once known
    read = 0
    written = 0
    next_grid = None
    out = None

    try:
        if snapshots:
            Path(snapshots).parent.mkdir(parents=True, exist_ok=True)
            out = open(snapshots, "w")
            out.write(snapshot_header(levels) + "\n")

        with open_feed(path) as f:
            for mtype, payload in itch.iter_messages(f):
                read += 1
                if limit is not None and read > limit:
                    read -= 1
                    break

                m = itch.decode(mtype, payload)
                if m is None:
                    continue  # a metadata type we don't model

                # Messages are chronological within a feed, so once past the
                # session end there is nothing later to see.
                if end_ns is not None and m["ts"] >= end_ns and m["ts"] > 0:
                    read -= 1
                    break

                if symbol is not None:
                    if m["type"] == "R" and m["stock"] == symbol:
                        locate = m["locate"]
                    # System events carry no meaningful locate but bracket the
                    # session, so let them through.
                    if m["type"] != "S" and (locate is None or m["locate"] != locate):
                        continue

                ts = m["ts"]
                if out is not None and ts > 0:
                    if next_grid is None:
                        next_grid = (ts // interval_ns + 1) * interval_ns
                    while ts >= next_grid:
                        out.write(snapshot_row(book, next_grid, levels) + "\n")
                        written += 1
                        next_grid += interval_ns

                book.apply(m)

        if symbol is not None and locate is None:
            raise SystemExit(f"error: symbol '{symbol}' not found in Stock Directory")
    finally:
        if out is not None:
            out.close()

    return book, read, written


def summary_dict(book, symbol, read, written):
    """The daily summary as plain data, for validate.py to compare against an
    external oracle. Prices stay in Price(4) integer units — converting to
    floats here would introduce rounding into a comparison that should be exact.
    """
    bid, ask = book.best_bid(), book.best_ask()
    return {
        "symbol": symbol,
        "messages_read": read,
        "messages_applied": book.applied,
        "unknown_refs": book.unknown_ref,
        "snapshots_written": written,
        "volume": book.volume,
        "hidden_volume": book.hidden_volume,
        "cross_volume": book.cross_volume,
        "trades": book.trades,
        "open": book.open,
        "high": book.high,
        "low": book.low,
        "close": book.close,
        "notional": book.notional,
        "vwap": book.vwap(),
        "cross_prices": book.cross_prices,
        "resting_orders": len(book.orders),
        "resting_shares": book.total_shares(),
        "best_bid": bid,
        "best_ask": ask,
        "crossed": book.crossed(),
        "system_event": book.system_event,
        "trading_state": book.trading_state,
    }


def print_summary(book, symbol, read, written, snapshots):
    bid, ask = book.best_bid(), book.best_ask()
    rows = [
        ("symbol", symbol or "(whole file)"),
        ("messages read", f"{read:,}"),
        ("messages applied", f"{book.applied:,}"),
        ("unknown order refs", f"{book.unknown_ref:,}"),
        ("", ""),
        ("volume (shares)", f"{book.volume:,}"),
        ("  of which hidden ('P')", f"{book.hidden_volume:,}"),
        ("  of which cross ('Q')", f"{book.cross_volume:,}"),
        ("trades (printable)", f"{book.trades:,}"),
        ("open", px(book.open)),
        ("high", px(book.high)),
        ("low", px(book.low)),
        ("close", px(book.close)),
        ("vwap", px(book.vwap())),
        ("", ""),
        ("resting orders", f"{len(book.orders):,}"),
        ("resting shares", f"{book.total_shares():,}"),
        ("best bid", px(bid)),
        ("best ask", px(ask)),
        ("book crossed", "YES - BUG" if book.crossed() else "no"),
        ("last system event", book.system_event or "-"),
        ("trading state", book.trading_state or "-"),
    ]
    if snapshots:
        rows += [("", ""), ("snapshots written", f"{written:,} -> {snapshots}")]

    print(f"{'field':<26} {'value':>18}")
    print("-" * 45)
    for k, v in rows:
        print("-" * 45 if k == "" else f"{k:<26} {v:>18}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed", help="ITCH file, .gz or raw")
    ap.add_argument("--symbol", help="ticker to reconstruct; omit for a single-symbol slice")
    ap.add_argument("--snapshots", help="write the snapshot CSV here")
    ap.add_argument("--interval-ms", type=float, default=60_000.0,
                    help="snapshot interval in milliseconds (default 60000 = 1 min)")
    ap.add_argument("--levels", type=int, default=10,
                    help="book levels per side in each snapshot (default 10)")
    ap.add_argument("--limit", type=int, help="stop after N messages")
    ap.add_argument("--json", metavar="PATH",
                    help="also write the summary as JSON, for validate.py")
    ap.add_argument("--end-ns", type=int,
                    help="stop at this timestamp (ns since ET midnight)")
    ap.add_argument("--utc-day", metavar="YYYY-MM-DD",
                    help="stop where the UTC day rolls over, which is how "
                         "vendors bucket daily bars — use this when the output "
                         "is going to be compared against one")
    ap.add_argument("--quiet", action="store_true", help="suppress the summary")
    a = ap.parse_args(argv)

    interval_ns = int(a.interval_ms * 1_000_000)
    if interval_ns <= 0:
        ap.error("--interval-ms must be positive")
    if a.levels <= 0:
        ap.error("--levels must be positive")

    end_ns = a.end_ns
    if a.utc_day:
        if end_ns is not None:
            ap.error("--end-ns and --utc-day are mutually exclusive")
        end_ns = utc_day_end_ns(a.utc_day)

    book, read, written = replay(a.feed, symbol=a.symbol, snapshots=a.snapshots,
                                 interval_ns=interval_ns, levels=a.levels,
                                 limit=a.limit, end_ns=end_ns)
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        summary = summary_dict(book, a.symbol, read, written)
        # Record the window, so a comparison against an external bar can check
        # it is being graded like for like instead of silently disagreeing.
        summary["session_end_ns"] = end_ns
        with open(a.json, "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
    if not a.quiet:
        print_summary(book, a.symbol, read, written, a.snapshots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
