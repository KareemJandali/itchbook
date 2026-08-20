#!/usr/bin/env python3
"""Grade a reconstruction against Databento's published daily bar.

This is the phase 2 done-condition. Everything else in this repo checks the code
against itself: the unit tests against hand-derived numbers, the differential
test against our own oracle. Both would pass happily if our reading of the ITCH
spec were wrong in the same way in both implementations. Only an outside number
settles that.

    # 1. reconstruct the symbol from a raw ITCH day
    ./build/itch_slice data/raw/<day>.gz MSFT data/sliced/MSFT.gz
    python3 python/reference/replay.py data/sliced/MSFT.gz \\
        --json data/sliced/MSFT.json

    # 2. grade it
    export DATABENTO_API_KEY=db-...
    python3 python/analysis/validate.py data/sliced/MSFT.json \\
        --symbol MSFT --date 2019-01-30

Why Databento's XNAS.ITCH and not a retail quote source: their bar is built from
the same NASDAQ feed we parsed, so it covers the same executions. A consolidated
figure from Yahoo or Google spans every US venue and dark pool, and NASDAQ is
only a fraction of that — comparing against one will look like a catastrophic
failure when the code is fine.

Note that Databento serves normalised DBN, not raw ITCH, so this provides the
oracle but not the input: the feed itself still has to come from NASDAQ or
another source of the original binary.

`--cost-only` prints what the query would bill before it spends anything.
"""
import argparse
import datetime
import json
import os
import sys

# Databento prices are 1e-9 fixed point; ITCH Price(4) is 1e-4. The ratio is
# exact, so the conversion stays in integers and the comparison stays exact.
DBN_PER_ITCH_TICK = 100_000

FIELDS = [
    ("volume", "volume (shares)", False),
    ("open", "open", True),
    ("high", "high", True),
    ("low", "low", True),
    ("close", "close", True),
]


def next_day(date):
    """The day after `date`, as YYYY-MM-DD.

    Databento's time ranges are half-open — [start, end). Passing the same date
    for both requests an empty range and comes back with no data, which reads
    like a missing trading day rather than a bad query.
    """
    d = datetime.date.fromisoformat(date)
    return (d + datetime.timedelta(days=1)).isoformat()


def to_itch_price(dbn_fixed):
    """1e-9 fixed point -> Price(4), or None if it does not divide evenly.

    A non-exact conversion means the oracle is quoting a sub-Price(4) tick, and
    silently rounding it would turn a real disagreement into a false pass.
    """
    if dbn_fixed is None:
        return None
    v = int(dbn_fixed)
    if v % DBN_PER_ITCH_TICK != 0:
        return None
    return v // DBN_PER_ITCH_TICK


def px(price):
    return "-" if price is None else f"{price / 10000:.4f}"


def compare(ours, theirs):
    """Compare two summaries field by field.

    `ours` is replay.py's --json output; `theirs` is the oracle, both in
    Price(4) units. Returns (rows, ok) where each row is
    (label, ours, theirs, delta, verdict).
    """
    rows = []
    ok = True
    for key, label, is_price in FIELDS:
        a = ours.get(key)
        b = theirs.get(key)
        if b is None:
            rows.append((label, a, None, None, "NO DATA"))
            continue
        if a is None:
            rows.append((label, None, b, None, "MISSING"))
            ok = False
            continue
        delta = a - b
        verdict = "match" if delta == 0 else "MISMATCH"
        if delta != 0:
            ok = False
        fmt = px if is_price else (lambda v: f"{v:,}")
        rows.append((label, fmt(a), fmt(b), delta, verdict))
    return rows, ok


def report(rows, ok, symbol, date):
    print(f"\n{symbol} {date} — reconstruction vs Databento XNAS.ITCH\n")
    print(f"{'field':<18} {'ours':>16} {'oracle':>16} {'delta':>14}  verdict")
    print("-" * 74)
    for label, a, b, delta, verdict in rows:
        d = "" if delta is None else f"{delta:,}"
        print(f"{label:<18} {str(a):>16} {str(b):>16} {d:>14}  {verdict}")
    print("-" * 74)
    if ok:
        print("\nPASS — the reconstruction matches the published bar.")
        print("Phase 2's done-condition is met for this symbol and date.")
    else:
        print("\nFAIL — see docs/build-plan.md section 4 for the usual causes.")
        print("  short a few percent, book looks right -> 'P'/'Q' volume not counted")
        print("  short by a lot                        -> locate filter dropping messages")
        print("  slightly over                         -> non-printable 'C' being counted")
        print("  OHLC off but volume right             -> the opening/closing crosses")
    return ok


def fetch_daily(key, dataset, symbol, date):
    """Pull the daily bar from Databento. The only part of this file that needs
    the network, kept in one place so everything else can be tested offline."""
    try:
        import databento as db
    except ImportError:
        raise SystemExit("error: pip install databento")

    client = db.Historical(key=key)
    data = client.timeseries.get_range(
        dataset=dataset,
        schema="ohlcv-1d",
        symbols=[symbol],
        stype_in="raw_symbol",
        start=date,
        end=next_day(date),
    )
    df = data.to_df(price_type="fixed")
    if len(df) == 0:
        raise SystemExit(f"error: Databento returned no {dataset} bar for "
                         f"{symbol} on {date} — check the date is a trading day "
                         f"and within the dataset's available range")
    row = df.iloc[0]
    out = {"volume": int(row["volume"])}
    for f in ("open", "high", "low", "close"):
        converted = to_itch_price(row[f])
        if converted is None:
            raise SystemExit(f"error: oracle {f} {row[f]} is not a whole Price(4) "
                             f"tick; refusing to round it into a false match")
        out[f] = converted
    return out


def query_cost(key, dataset, symbol, date):
    try:
        import databento as db
    except ImportError:
        raise SystemExit("error: pip install databento")
    client = db.Historical(key=key)
    return client.metadata.get_cost(
        dataset=dataset, schema="ohlcv-1d", symbols=[symbol],
        stype_in="raw_symbol", start=date, end=next_day(date),
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("summary", help="replay.py --json output")
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--date", required=True, help="the trading day, YYYY-MM-DD")
    ap.add_argument("--dataset", default="XNAS.ITCH")
    ap.add_argument("--api-key", default=os.environ.get("DATABENTO_API_KEY"),
                    help="defaults to $DATABENTO_API_KEY")
    ap.add_argument("--cost-only", action="store_true",
                    help="print what the query would bill and stop")
    ap.add_argument("--oracle-json", metavar="PATH",
                    help="read the oracle from a file instead of the network, "
                         "for offline reruns of a fetch you already paid for")
    a = ap.parse_args(argv)

    with open(a.summary) as f:
        ours = json.load(f)

    if a.oracle_json:
        with open(a.oracle_json) as f:
            theirs = json.load(f)
    else:
        if not a.api_key:
            raise SystemExit("error: no API key — pass --api-key or set "
                             "DATABENTO_API_KEY")
        if a.cost_only:
            cost = query_cost(a.api_key, a.dataset, a.symbol, a.date)
            print(f"{a.dataset} ohlcv-1d {a.symbol} {a.date}: ${cost}")
            return 0
        theirs = fetch_daily(a.api_key, a.dataset, a.symbol, a.date)

    rows, ok = compare(ours, theirs)
    return 0 if report(rows, ok, a.symbol, a.date) else 1


if __name__ == "__main__":
    sys.exit(main())
