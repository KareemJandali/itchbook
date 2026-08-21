#!/usr/bin/env python3
"""Generate a small, spec-shaped synthetic ITCH 5.0 file for smoke-testing.

This is NOT real market data — it exists so the tools and the reference book
produce sane output without downloading a multi-GB NASDAQ day. Real validation
happens against a real feed and the LOBSTER/NASDAQ oracle; see README.

The feed is deliberately shaped like a market and not like noise:

  * a two-sided, uncrossed book — bids below the mid, asks above it
  * every message type the reference book models: A F E C X D U P Q H R S
  * one non-printable 'C', which must move the book but not the volume
  * an opening and a closing cross, which move volume but not the book

`tests/test_reference.py` asserts the exact book state and daily statistics this
produces, so changing the numbers here means updating that test.

    python3 python/make_sample.py data/raw/sample.gz

Then:
    ./build/itch_census data/raw/sample.gz
    ./build/itch_dump   data/raw/sample.gz 10
    ./build/itch_slice  data/raw/sample.gz TEST data/sliced/TEST.gz
    python3 python/reference/replay.py data/raw/sample.gz --symbol TEST
"""
import gzip
import struct
import sys
from pathlib import Path

LOC = 1                 # stock locate code for TEST
SYMBOL = "TEST"
MID = 100_0000          # $10.0000 in Price(4)
TICK = 100              # $0.01
LEVELS = 20             # price levels per side
BID_REF = 1000          # ref numbers: bids 1000.., asks 2000..
ASK_REF = 2000
T0 = 34_200_000_000_000  # ~09:30:00 in ns since midnight


def frame(payload: bytes) -> bytes:
    """Prefix a payload with its 2-byte big-endian length."""
    return struct.pack(">H", len(payload)) + payload


def ts6(ns: int) -> bytes:
    """6-byte big-endian nanoseconds-since-midnight."""
    return ns.to_bytes(6, "big")


def header(mtype: bytes, ns: int) -> bytes:
    """type(1) locate(2) tracking(2) timestamp(6) — common to every message."""
    return mtype + struct.pack(">HH", LOC, 0) + ts6(ns)


def stock(symbol: str) -> bytes:
    """8 chars, left-justified, space-padded."""
    return symbol.encode().ljust(8)[:8]


def system_event(ns, code):  # 'S', 12
    return header(b"S", ns) + code


def stock_directory(ns, symbol):  # 'R', 39
    body = header(b"R", ns) + stock(symbol)
    return body + b"\x00" * (39 - len(body))


def add_order(ns, ref, side, shares, price, symbol=SYMBOL):  # 'A', 36
    return (header(b"A", ns) + struct.pack(">Q", ref) + side +
            struct.pack(">I", shares) + stock(symbol) + struct.pack(">I", price))


def add_order_mpid(ns, ref, side, shares, price, mpid, symbol=SYMBOL):  # 'F', 40
    # Identical to 'A' after the type byte, plus a 4-char attribution. Build the
    # header separately: reusing add_order() would stamp an 'A' type byte on a
    # 40-byte message, and the length-prefix check would (rightly) reject it.
    return (header(b"F", ns) + struct.pack(">Q", ref) + side +
            struct.pack(">I", shares) + stock(symbol) + struct.pack(">I", price) +
            mpid.encode().ljust(4)[:4])


def order_executed(ns, ref, shares, match):  # 'E', 31
    return (header(b"E", ns) + struct.pack(">Q", ref) +
            struct.pack(">I", shares) + struct.pack(">Q", match))


def order_executed_price(ns, ref, shares, match, printable, price):  # 'C', 36
    return (header(b"C", ns) + struct.pack(">Q", ref) +
            struct.pack(">I", shares) + struct.pack(">Q", match) +
            printable + struct.pack(">I", price))


def order_cancel(ns, ref, shares):  # 'X', 23
    return header(b"X", ns) + struct.pack(">Q", ref) + struct.pack(">I", shares)


def order_delete(ns, ref):  # 'D', 19
    return header(b"D", ns) + struct.pack(">Q", ref)


def order_replace(ns, orig_ref, new_ref, shares, price):  # 'U', 35
    return (header(b"U", ns) + struct.pack(">QQ", orig_ref, new_ref) +
            struct.pack(">II", shares, price))


def trade(ns, ref, side, shares, price, match, symbol=SYMBOL):  # 'P', 44
    return (header(b"P", ns) + struct.pack(">Q", ref) + side +
            struct.pack(">I", shares) + stock(symbol) +
            struct.pack(">I", price) + struct.pack(">Q", match))


def cross_trade(ns, shares, price, match, cross_type, symbol=SYMBOL):  # 'Q', 40
    return (header(b"Q", ns) + struct.pack(">Q", shares) + stock(symbol) +
            struct.pack(">I", price) + struct.pack(">Q", match) + cross_type)


def trading_action(ns, state, reason="", symbol=SYMBOL):  # 'H', 25
    return (header(b"H", ns) + stock(symbol) + state + b" " +
            reason.encode().ljust(4)[:4])


# ---- the three that do not touch a book, and had no generator -----------------
#
# From the repository's own census note: "Nothing in this repository generates
# those types, so CI cannot reach them." These three are the ones that matter —
# two decide whether a symbol may trade and the third decides whether a day's
# volume is final — so they get builders, and fuzz_feed.py emits them.
#
# What that does and does not prove, stated plainly because it is easy to
# overclaim: building a message from the same length constant that parses it
# says NOTHING about whether the constant matches NASDAQ's wire. It exercises
# the handling path — the routing, the counters, the tradability derivation —
# which had no coverage at all and could rot silently between here and the day
# someone runs a file that contains one. The constants themselves stay
# unconfirmed until a real message meets them.


def operational_halt(ns, action, market=b"Q", symbol=SYMBOL):  # 'h', 21
    return header(b"h", ns) + stock(symbol) + market + action


def mwcb_status(ns, level):  # 'W', 12
    return header(b"W", ns) + level


def broken_trade(ns, match):  # 'B', 19
    return header(b"B", ns) + struct.pack(">Q", match)


def build() -> bytes:
    out = bytearray()
    t = T0
    match = 1

    def emit(payload):
        out.extend(frame(payload))

    emit(system_event(t, b"O"))                       # start of messages
    emit(stock_directory(t, SYMBOL))
    emit(system_event(t, b"Q"))                       # market open

    # Opening cross: real volume, no book effect.
    emit(cross_trade(t, 5000, MID, match, b"O")); match += 1

    # A two-sided book: 20 levels a side, bids below the mid and asks above it.
    for i in range(LEVELS):
        t += 1_000_000
        emit(add_order(t, BID_REF + i, b"B", 100 + i, MID - (i + 1) * TICK))
        emit(add_order(t, ASK_REF + i, b"S", 200 + i, MID + (i + 1) * TICK))

    # 'F' — an attributed add joining the back of the best bid.
    t += 1_000_000
    emit(add_order_mpid(t, 3000, b"B", 500, MID - TICK, "NSDQ"))

    # 'E' — partial execution against the front of the best bid, at its own price.
    t += 1_000_000
    emit(order_executed(t, BID_REF + 0, 40, match)); match += 1

    # 'C' — execution at a stated price, printable: counts toward volume, and
    # the order keeps resting at its own price.
    t += 1_000_000
    emit(order_executed_price(t, BID_REF + 1, 50, match, b"Y", MID - 150)); match += 1

    # 'C' non-printable — removes shares from the book but must NOT count toward
    # volume, VWAP or OHLC. This is the one that catches a lazy implementation.
    t += 1_000_000
    emit(order_executed_price(t, BID_REF + 2, 30, match, b"N", MID - 250)); match += 1

    # 'X' partial cancel, 'D' full delete, 'U' replace (loses queue priority).
    t += 1_000_000
    emit(order_cancel(t, ASK_REF + 0, 50))
    t += 1_000_000
    emit(order_delete(t, ASK_REF + 1))
    t += 1_000_000
    emit(order_replace(t, ASK_REF + 2, 4000, 300, MID + 5 * TICK))

    # 'P' — hidden liquidity trading at the mid. Volume, no book effect.
    t += 1_000_000
    emit(trade(t, 5000, b"B", 250, MID, match)); match += 1

    # 'H' — trading action.
    t += 1_000_000
    emit(trading_action(t, b"T"))

    # Closing cross, then the session-end markers.
    t += 1_000_000
    emit(cross_trade(t, 3000, MID + 2 * TICK, match, b"C")); match += 1
    emit(system_event(t, b"M"))                       # market close
    emit(system_event(t, b"C"))                       # end of messages
    return bytes(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/raw/sample.gz"
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    data = build()
    with gzip.open(path, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes of framed ITCH to {path}")


if __name__ == "__main__":
    main()
