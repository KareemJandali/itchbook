#!/usr/bin/env python3
"""Check the reconstructed auction prices against NASDAQ's published ones.

    python3 python/analysis/check_cross.py validation/MSFT_2019-12-30.json \
        --official-close 157.59 --official-open 158.99

The build plan wants the reconstruction matched against "NASDAQ's published
daily summary". Two of the routes to one are closed:

  * A CONSOLIDATED daily bar — what most sources publish — is every venue at
    once. ITCH is one venue. MSFT traded roughly 20M shares that day across the
    market and 6.2M of them on NASDAQ, so comparing the two fails for an
    entirely correct reason.
  * NasdaqTrader's per-symbol matched volume is venue-specific and would be
    right, but the site keeps a rolling window and will not go back to 2019.

What survives is better than either, because it needs no download and no
subscription: the OFFICIAL OPENING AND CLOSING PRICES. Both are auction prints,
both arrive in the feed as 'Q' cross trades, and for a NASDAQ-listed stock the
official closing price *is* the closing cross. Every quote history in the world
publishes it. If our reconstruction of that auction agrees with the number
NASDAQ published, our cross handling is right — and cross handling is the part
of an ITCH book most likely to be quietly wrong, because it is rare, it is a
different message type, and it never shows up in a day's ordinary flow.

Note this is a different number from the summary's `close`, which is the last
trade of the session including late prints. The auction is the auction.
"""
import argparse
import json
import sys
from pathlib import Path

# 'O' opening cross, 'C' closing cross. 'H' is a halt-resumption cross and 'I'
# an intraday/IPO one; neither has a published daily figure to check against.
CHECKED = [("O", "official open", "official_open"),
           ("C", "official close", "official_close")]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("summary", help="replay.py / book_replay --json output")
    ap.add_argument("--official-open", type=float,
                    help="NASDAQ official opening price, in dollars")
    ap.add_argument("--official-close", type=float,
                    help="NASDAQ official closing price, in dollars")
    ap.add_argument("--source", default="a published quote history",
                    help="where the official prices came from, for the record")
    a = ap.parse_args()

    d = json.loads(Path(a.summary).read_text())
    crosses = d.get("cross_prices") or {}
    if not crosses:
        print("no cross prints in this summary — nothing to check.")
        print("A feed sliced to continuous hours only will have none.")
        return 1

    print(f"cross prints reconstructed from the feed: "
          f"{', '.join(sorted(crosses))}\n")
    print(f"{'auction':<16}{'ours':>12}{'published':>12}  verdict")
    print("-" * 52)

    checked = 0
    bad = 0
    for code, label, arg in CHECKED:
        want = getattr(a, arg)
        if code not in crosses:
            continue
        ours = crosses[code] / 10000.0
        if want is None:
            print(f"{label:<16}{ours:>12.4f}{'—':>12}  not checked")
            continue
        checked += 1
        # Published prices carry cents; ours carry four decimals. Compare at
        # the precision the published figure actually has.
        same = abs(round(ours, 2) - round(want, 2)) < 1e-9
        if not same:
            bad += 1
        print(f"{label:<16}{ours:>12.4f}{want:>12.2f}  {'match' if same else 'DIFFER'}")

    print("-" * 52)
    if checked == 0:
        print("\nNothing was checked. Pass --official-open and/or "
              "--official-close.")
        print("For a NASDAQ-listed stock those are the open and close every "
              "quote")
        print("history reports for the date.")
        return 1
    if bad:
        print(f"\nFAIL — {bad} of {checked} auction prices disagree with "
              f"{a.source}.")
        print("  both off by the same amount  -> a price-scaling bug (Price(4) "
              "is 1e-4)")
        print("  only the close is wrong      -> the closing cross is being "
              "missed or double-counted")
        print("  only the open is wrong       -> pre-market prints are being "
              "treated as the auction")
        return 1
    print(f"\nPASS — {checked} auction price(s) match {a.source}.")
    print("The cross prints are reconstructed correctly, against a NASDAQ "
          "figure")
    print("that needs no subscription to verify.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
