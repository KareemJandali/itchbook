#!/usr/bin/env python3
"""Extract one symbol's messages from a full ITCH day — pure Python, no build.

`tools/itch_slice.cpp` does this faster, but this needs nothing but Python, so a
full day can be cut down on any machine that can download it. A multi-GB day
becomes a few MB for one symbol — small enough to commit and hand to someone
else.

    python3 python/slice_symbol.py 01302019.NASDAQ_ITCH50.gz MSFT MSFT.gz

Two passes, same as the C++ version: read the Stock Directory ('R') block to map
the ticker to its stock locate code, then keep every message carrying that code.
System events ('S') are kept regardless — they bracket the session.
"""
import argparse
import gzip
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from reference import parser as itch   # noqa: E402


def open_feed(path):
    return gzip.open(path, "rb") if str(path).endswith(".gz") else open(path, "rb")


def find_locate(path, symbol):
    """Pass 1 — the ticker's stock locate code, from the 'R' block at the top."""
    want = symbol.upper()
    with open_feed(path) as f:
        for mtype, payload in itch.iter_messages(f):
            if mtype != b"R":
                continue
            if itch._stock(payload, 11) == want:
                return itch.stock_locate(payload)
    return None


def slice_symbol(path, symbol, out_path, locate):
    """Pass 2 — keep messages on that locate, in the same [len][payload] framing."""
    kept = total = 0
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    with open_feed(path) as f, gzip.open(out_path, "wb") as out:
        for mtype, payload in itch.iter_messages(f):
            total += 1
            if mtype != b"S" and itch.stock_locate(payload) != locate:
                continue
            out.write(struct.pack(">H", len(payload)))
            out.write(payload)
            kept += 1
            if total % 5_000_000 == 0:
                print(f"  ...{total:,} messages scanned, {kept:,} kept", flush=True)
    return kept, total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed", help="the full ITCH day")
    ap.add_argument("symbol")
    ap.add_argument("out", help="where to write the slice (.gz)")
    a = ap.parse_args()

    print(f"pass 1: looking up {a.symbol} in the Stock Directory...", flush=True)
    locate = find_locate(a.feed, a.symbol)
    if locate is None:
        raise SystemExit(f"error: '{a.symbol}' not found — check the ticker")
    print(f"  {a.symbol} -> stock_locate {locate}", flush=True)

    print("pass 2: extracting (this reads the whole file, be patient)...", flush=True)
    kept, total = slice_symbol(a.feed, a.symbol, a.out, locate)
    size_mb = Path(a.out).stat().st_size / 1e6
    print(f"\nwrote {kept:,} of {total:,} messages to {a.out} ({size_mb:.1f} MB)")
    if size_mb > 90:
        print("WARNING: over ~90MB — GitHub rejects files above 100MB.")
        print("         Pick a less active symbol.")


if __name__ == "__main__":
    main()
