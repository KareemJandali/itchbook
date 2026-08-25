#!/usr/bin/env python3
"""fetch_auction_stats.py — the venue's own auction prices, as an oracle.

    python3 python/analysis/fetch_auction_stats.py --cost-only
    python3 python/analysis/fetch_auction_stats.py --spend

WHY THIS EXISTS, AND WHAT IT REPLACES.

check_cross.py grades our reconstructed cross prints against published auction
prices, and until now those prices were read by hand off a consolidated quote
history. That works for the CLOSE and not for the OPEN, and the difference is
not a detail:

  * For a NASDAQ-listed stock the official closing price IS the closing cross,
    so every quote history in the world reports the same number we do.
  * The OPEN column of a consolidated daily bar is the first print across every
    US venue, which is a different quantity from the NASDAQ opening cross and
    agrees with it only by coincidence.

That coincidence held four times out of five and failed on MSFT 2019-08-30 --
ours 139.1000, the consolidated open 139.15 -- and the volumes say why: Yahoo
reports 23,940,100 shares for that day where XNAS.ITCH carries 9,674,474. A
2.47x difference is not two views of one tape. check_cross.py's own docstring
had already rejected consolidated VOLUME for exactly this reason and then used a
consolidated bar's open anyway.

Databento's `statistics` schema on XNAS.ITCH carries the venue's own published
auction prices: same universe as the feed we parse, and the auction itself
rather than a first-trade proxy. It costs about two millionths of a dollar per
symbol-day, which is cheaper than the ohlcv-1d bars already committed beside it.

PAY ONCE. Each response is written to validation/databento-stats-<SYM>-<DATE>.json
so the grading replays offline forever after, the same contract the ohlcv-1d
oracle already follows.
"""
import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DATASET = "XNAS.ITCH"

# The graded basket, both days. Same list as scripts/databento-grade.sh.
BASKET = [
    ("MSFT", "2019-12-30"), ("QQQ", "2019-12-30"), ("ALLE", "2019-12-30"),
    ("AQB", "2019-12-30"), ("MKD", "2019-12-30"),
    ("MSFT", "2019-08-30"), ("QQQ", "2019-08-30"), ("ALLE", "2019-08-30"),
    ("AQB", "2019-08-30"), ("ELTK", "2019-08-30"),
]


def next_day(d):
    return (dt.date.fromisoformat(d) + dt.timedelta(days=1)).isoformat()


def client_or_die(key):
    if not key:
        raise SystemExit("error: DATABENTO_API_KEY is not set")
    try:
        import databento as db
    except ImportError:
        raise SystemExit("error: the databento package is not on this python.\n"
                         "       python3 -m venv ~/dbenv && ~/dbenv/bin/pip install databento")
    return db, db.Historical(key=key)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cost-only", action="store_true",
                    help="price the whole basket and stop; bills nothing")
    ap.add_argument("--spend", action="store_true",
                    help="actually fetch. Without it nothing is bought.")
    ap.add_argument("--api-key", default=os.environ.get("DATABENTO_API_KEY"))
    ap.add_argument("--out-dir", default=str(ROOT / "validation"))
    a = ap.parse_args(argv)

    db, client = client_or_die(a.api_key)

    if not a.spend:
        total = 0.0
        print("=== what the statistics basket would bill ===")
        for sym, day in BASKET:
            try:
                c = float(client.metadata.get_cost(
                    dataset=DATASET, schema="statistics", symbols=[sym],
                    stype_in="raw_symbol", start=day, end=next_day(day)))
                total += c
                print(f"  {sym:<5} {day}   ${c:.8f}")
            except Exception as e:
                print(f"  {sym:<5} {day}   error: {e}")
        print(f"\n  ten symbol-days: ${total:.8f}")
        print("\nNothing was spent. Re-run with --spend to fetch.")
        return 0

    # StatType names, so the artifact records what each row MEANS rather than a
    # bare integer that a later reader has to look up and might guess at.
    # The first version built this from `db.StatType` and swallowed the failure,
    # so every row came out "UNKNOWN_<StatType.OPENING_PRICE: 1>" -- the name was
    # right there inside the string it was calling unknown. Parse the repr,
    # which is what actually survives serialisation, and fall back to the enum
    # only if that fails.
    names = {}
    try:
        for st in db.StatType:
            names[int(st)] = st.name
    except Exception:
        pass

    def type_name(v):
        m = re.search(r"StatType\.(\w+)", str(v))
        if m:
            return m.group(1)
        m = re.search(r"(\d+)", str(v))
        return names.get(int(m.group(1)), f"UNKNOWN_{v}") if m else f"UNKNOWN_{v}"

    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    seen_types = {}
    for sym, day in BASKET:
        dest = out_dir / f"databento-stats-{sym}-{day}.json"
        if dest.exists():
            print(f"  {sym:<5} {day}   already committed, not re-buying")
            continue
        try:
            data = client.timeseries.get_range(
                dataset=DATASET, schema="statistics", symbols=[sym],
                stype_in="raw_symbol", start=day, end=next_day(day))
            rows = []
            for rec in data:
                d = {}
                for f in ("ts_event", "ts_ref", "price", "quantity",
                          "stat_type", "update_action", "sequence"):
                    v = getattr(rec, f, None)
                    if v is not None:
                        d[f] = int(v) if isinstance(v, (int,)) else v
                st = d.get("stat_type")
                if st is not None:
                    d["stat_type_name"] = type_name(st)
                    m = re.search(r"(\d+)", str(st))
                    if m:
                        seen_types[int(m.group(1))] = d["stat_type_name"]
                rows.append(d)
            payload = {"dataset": DATASET, "schema": "statistics",
                       "symbol": sym, "date": day, "records": rows}
            dest.write_text(json.dumps(payload, indent=2, default=str) + "\n")
            print(f"  {sym:<5} {day}   {len(rows):>4} records -> {dest.name}")
        except Exception as e:
            print(f"  {sym:<5} {day}   error: {e}")

    if seen_types:
        print("\n=== stat types this dataset actually emitted ===")
        for k in sorted(seen_types):
            print(f"  {k:>4}  {seen_types[k]}")
        print("\nThe auction prices are whichever of these name the opening and")
        print("closing price. Nothing is graded yet -- that is check_cross.py's")
        print("job, and it now has a same-universe source to do it against.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
