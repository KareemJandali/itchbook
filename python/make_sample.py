#!/usr/bin/env python3
"""Generate a small, spec-shaped synthetic ITCH 5.0 file for smoke-testing.

This is NOT real market data — it exists so the tools produce sane output
without downloading a multi-GB NASDAQ day. Real validation happens against a
real feed + the LOBSTER/NASDAQ oracle in Phase 2.

    python3 python/make_sample.py data/raw/sample.gz

Then:
    ./build/itch_census data/raw/sample.gz
    ./build/itch_dump   data/raw/sample.gz 10
    ./build/itch_slice  data/raw/sample.gz TEST data/sliced/TEST.gz
"""
import gzip
import struct
import sys


def frame(payload: bytes) -> bytes:
    """Prefix a payload with its 2-byte big-endian length."""
    return struct.pack(">H", len(payload)) + payload


def ts6(ns: int) -> bytes:
    """6-byte big-endian nanoseconds-since-midnight."""
    return ns.to_bytes(6, "big")


def system_event(locate, ns, code):  # 'S', 12 bytes
    return b"S" + struct.pack(">HH", locate, 0) + ts6(ns) + code


def stock_directory(locate, ns, symbol):  # 'R', 39 bytes
    stock = symbol.encode().ljust(8)[:8]
    body = b"R" + struct.pack(">HH", locate, 0) + ts6(ns) + stock
    return body + b"\x00" * (39 - len(body))


def add_order(locate, ns, ref, side, shares, symbol, price):  # 'A', 36 bytes
    stock = symbol.encode().ljust(8)[:8]
    return (b"A" + struct.pack(">HH", locate, 0) + ts6(ns) +
            struct.pack(">Q", ref) + side + struct.pack(">I", shares) +
            stock + struct.pack(">I", price))


def order_executed(locate, ns, ref, shares, match):  # 'E', 31 bytes
    return (b"E" + struct.pack(">HH", locate, 0) + ts6(ns) +
            struct.pack(">Q", ref) + struct.pack(">I", shares) +
            struct.pack(">Q", match))


def order_cancel(locate, ns, ref, shares):  # 'X', 23 bytes
    return (b"X" + struct.pack(">HH", locate, 0) + ts6(ns) +
            struct.pack(">Q", ref) + struct.pack(">I", shares))


def order_delete(locate, ns, ref):  # 'D', 19 bytes
    return (b"D" + struct.pack(">HH", locate, 0) + ts6(ns) + struct.pack(">Q", ref))


def build() -> bytes:
    LOC = 1
    out = bytearray()
    t = 34_200_000_000_000  # ~09:30:00 in ns since midnight

    out += frame(system_event(LOC, t, b"O"))          # start of messages
    out += frame(stock_directory(LOC, t, "TEST"))
    out += frame(system_event(LOC, t, b"Q"))          # market open

    ref = 1000
    price = 100_0000  # $10.0000 in Price(4)
    match = 1
    for i in range(50):
        t += 1_000_000
        side = b"B" if i % 2 == 0 else b"S"
        out += frame(add_order(LOC, t, ref + i, side, 100 + i, "TEST", price + (i - 25) * 100))

    # execute, cancel, and delete a few of them
    for i in range(0, 20, 4):
        t += 500_000
        out += frame(order_executed(LOC, t, ref + i, 50, match)); match += 1
        out += frame(order_cancel(LOC, t, ref + i + 1, 25))
        out += frame(order_delete(LOC, t, ref + i + 2))

    out += frame(system_event(LOC, t, b"M"))          # market close
    out += frame(system_event(LOC, t, b"C"))          # end of messages
    return bytes(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/raw/sample.gz"
    data = build()
    with gzip.open(path, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes of framed ITCH to {path}")


if __name__ == "__main__":
    main()
