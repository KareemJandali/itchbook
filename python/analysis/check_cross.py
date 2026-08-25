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

AND THE OPEN COLUMN OF A QUOTE HISTORY IS NOT THE OPENING CROSS.

The paragraph above is right about the close and was wrong about the open, in a
way that took a venue-specific oracle to see. For a NASDAQ-listed stock the
official closing price IS the closing cross, so any quote history reports the
same number we do. The OPEN column of a consolidated daily bar is the first
print across every US venue, which is a different quantity and coincides with
the opening cross only by luck.

It coincided four times out of five and failed on MSFT 2019-08-30 -- ours
139.1000, the consolidated open 139.15 -- and the volumes said why: Yahoo
reported 23,940,100 shares for that day where XNAS.ITCH carries 9,674,474, and
Yahoo's high was LOWER than ours. Two views of one tape cannot do that. This
file had already rejected consolidated VOLUME for exactly that reason, four
paragraphs up, and then used a consolidated bar's open anyway.

So --stats-json is the oracle to prefer: Databento's `statistics` schema on
XNAS.ITCH, which carries the venue's own published OPENING_PRICE and
CLOSE_PRICE. Same universe as the feed we parse, the auction itself rather than
a first-trade proxy, and about two millionths of a dollar per symbol-day. It
settled MSFT 2019-08-30 at 139.1000 -- our reconstruction was right and the
oracle had been the wrong one.

It also grades the ABSENCES, which no consolidated source can. A symbol with no
NASDAQ auction gets UNDEF_PRICE from the venue rather than silence, so "we
reconstructed no opening cross and the venue published none" becomes a checked
agreement instead of an untested gap -- and "we invented an auction the venue
never held" becomes a failure instead of going unnoticed.
"""
import argparse
import json
import sys
from pathlib import Path

# 'O' opening cross, 'C' closing cross. 'H' is a halt-resumption cross and 'I'
# an intraday/IPO one; neither has a published daily figure to check against.
CHECKED = [("O", "official open", "official_open"),
           ("C", "official close", "official_close")]

# Databento's "there is no such statistic" sentinel, INT64_MAX. It is not a
# price and must never be read as one: at 1e-9 scaling it renders as
# 9,223,372,036.85, which is exactly the kind of number that looks like a bug in
# our book rather than an absence in theirs.
UNDEF_PRICE = 9223372036854775807
STAT_OPENING, STAT_CLOSE = 1, 11
STAT_TO_CROSS = {STAT_OPENING: "O", STAT_CLOSE: "C"}


def stat_type_int(v):
    """stat_type serialises as an int or as the enum's repr, depending on path."""
    if isinstance(v, int):
        return v
    import re
    m = re.search(r"(\d+)", str(v))
    return int(m.group(1)) if m else None


def venue_auctions(path):
    """{'O': dollars-or-None, 'C': ...} from a committed statistics artifact.

    None means the venue published UNDEF_PRICE -- it held no such auction. That
    is a fact to check against, not a missing input.
    """
    recs = json.loads(Path(path).read_text())["records"]
    out = {}
    for rec in recs:
        code = STAT_TO_CROSS.get(stat_type_int(rec.get("stat_type")))
        if code is None:
            continue
        raw = rec.get("price")
        out[code] = None if raw is None or int(raw) == UNDEF_PRICE else int(raw) / 1e9
    return out


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
    ap.add_argument("--stats-json", metavar="PATH",
                    help="a committed Databento `statistics` artifact for this "
                         "symbol-day — the venue's own published auction "
                         "prices. Supersedes --official-open/--official-close, "
                         "and is the oracle to prefer: see the note above on "
                         "why a quote history's open column is a different "
                         "quantity from the opening cross.")
    a = ap.parse_args()

    venue = None
    if a.stats_json:
        venue = venue_auctions(a.stats_json)
        a.source = f"the venue's own published auction prices ({a.stats_json})"

    d = json.loads(Path(a.summary).read_text())
    crosses = d.get("cross_prices") or {}
    # NO CROSS PRINTS IS AN ANSWER WHEN THERE IS SOMETHING TO CHECK IT AGAINST.
    # Without an oracle it is indistinguishable from a feed sliced to continuous
    # hours, so it stays an error. With one it is a claim -- "this symbol held no
    # NASDAQ auction" -- and the venue either agrees or does not. ALLE is the
    # case: NYSE-listed, no NASDAQ auction on either day, no cross prints in our
    # reconstruction, and UNDEF_PRICE from the venue for both. That is a pass,
    # and reporting it as "nothing to check" threw away the one row in the
    # basket that tests the absence.
    if not crosses and venue is None:
        print("no cross prints in this summary — nothing to check.")
        print("A feed sliced to continuous hours only will have none.")
        return 1

    print(f"cross prints reconstructed from the feed: "
          f"{', '.join(sorted(crosses)) if crosses else 'none'}\n")
    print(f"{'auction':<16}{'ours':>12}{'published':>12}  verdict")
    print("-" * 52)

    checked = 0
    bad = 0
    unusable = 0
    for code, label, arg in CHECKED:
        want = getattr(a, arg)

        # THE VENUE PATH ALSO GRADES ABSENCES. Four cases, and only one of them
        # is the ordinary comparison; the other three are things a consolidated
        # source cannot express at all.
        if venue is not None and code in venue:
            theirs = venue[code]
            mine = crosses[code] / 10000.0 if code in crosses else None
            if theirs is None and mine is None:
                checked += 1
                print(f"{label:<16}{'none':>12}{'none':>12}  agree: no such auction")
                continue
            if theirs is None:
                checked += 1; bad += 1
                print(f"{label:<16}{mine:>12.4f}{'none':>12}  WE INVENTED ONE")
                continue
            if mine is None:
                checked += 1; bad += 1
                print(f"{label:<16}{'none':>12}{theirs:>12.4f}  WE MISSED ONE")
                continue
            checked += 1
            same = abs(mine - theirs) < 5e-5
            bad += not same
            print(f"{label:<16}{mine:>12.4f}{theirs:>12.4f}  "
                  f"{'match' if same else 'DIFFER'}")
            continue

        if code not in crosses:
            continue
        ours = crosses[code] / 10000.0
        if want is None:
            print(f"{label:<16}{ours:>12.4f}{'—':>12}  not checked")
            continue
        # A figure that is not a whole number of cents cannot be a cross
        # price, so it cannot check one. NASDAQ's opening and closing crosses
        # clear at a single price built from orders that are themselves priced
        # in pennies — Reg NMS Rule 612 forbids sub-penny quoting at or above
        # $1.00 — so an auction at $158.987 is not a thing that happens.
        #
        # This is not pedantry. nasdaq.com's own historical-quotes CSV gives
        # MSFT's 2019-12-30 open as $158.987, and an earlier version of this
        # script compared at two decimals, rounded that to $158.99, and
        # reported a MATCH against our $158.9900. It was not a match. It was
        # two different quantities agreeing after the difference between them
        # had been rounded away — a check passing by discarding the precision
        # that would have shown its inputs were not comparable.
        if ours >= 1.0 and abs(round(want * 100) - want * 100) > 1e-6:
            print(f"{label:<16}{ours:>12.4f}{want:>12.4f}  NOT AN AUCTION PRICE")
            unusable += 1
            continue
        checked += 1
        # Both sides are now exact cent values, so compare them as such rather
        # than at some rounded precision.
        same = abs(ours - want) < 1e-6
        if not same:
            bad += 1
        print(f"{label:<16}{ours:>12.4f}{want:>12.2f}  {'match' if same else 'DIFFER'}")

    print("-" * 52)
    if unusable:
        print(f"\n{unusable} published figure(s) are not on a penny increment, so they are")
        print("not auction prices and cannot check one. That column of your source")
        print("means something else — most likely the first consolidated print of")
        print("the session, which can be sub-penny, rather than the opening cross.")
        print("Find a source that publishes the cross itself, or check the close")
        print("alone: for a NASDAQ-listed stock the official closing price IS the")
        print("closing cross, and every quote history reports it.")
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
    if a.stats_json:
        print("The cross prints are reconstructed correctly, against the "
              "venue's own")
        print("published auction prices — same universe as the feed, and the "
              "auction")
        print("itself rather than a first-trade proxy.")
    else:
        print("The cross prints are reconstructed correctly, against a NASDAQ "
              "figure")
        print("that needs no subscription to verify. Note the close is sound "
              "this way")
        print("and the open is not: see the note at the top of this file.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
