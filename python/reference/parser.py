"""Slow, obvious ITCH 5.0 parser — the oracle (Phase 2).

No cleverness. `struct.unpack` with big-endian format strings, fields read at
explicit byte offsets. This exists to be correct, not fast: it is the ground
truth the C++ implementation is diffed against for the next six months.

The offsets here mirror `include/itchbook/itch/messages.hpp` exactly. If the two
ever disagree, one of them is wrong and the differential test will say so.

Wire format reminders (the traps from the build plan):
  * Each message is preceded by a 2-byte big-endian length prefix. The file is
    not a bare message stream.
  * Everything is big-endian.
  * Timestamps are 6 bytes — nanoseconds since midnight, at offset 5 in every
    message that carries one. There is no 6-byte integer type.
  * Prices are Price(4): unsigned integers with 4 implied decimals.
"""
import struct

# ---- header ------------------------------------------------------------------
#
# Every message starts with: type(1) stock_locate(2) tracking(2) timestamp(6).
# The body therefore begins at offset 11.

HEADER_LEN = 11

# Payload length (including the type byte) for every ITCH 5.0 type, modelled or
# not. A prefix that disagrees with the table means we have desynced, and we
# want to know now rather than 200MB later.
#
# The unmodelled types are in here for the same reason they are in the C++
# spec_length(): the handler ignores them, but a desync that starts inside a
# metadata message is a desync all the same, and leaving them unchecked means
# noticing it only at the next message we happen to care about. This table must
# stay identical to include/itchbook/itch/messages.hpp — the two parsers are a
# differential test of each other, and a table that drifts turns that test into
# noise.
SPEC_LENGTH = {
    # Modelled.
    b"S": 12, b"R": 39, b"A": 36, b"F": 40, b"E": 31,
    b"C": 36, b"X": 23, b"D": 19, b"U": 35, b"P": 44, b"Q": 40, b"H": 25,
    # Framed and length-checked, but not interpreted. See messages.hpp for what
    # each one is and why skipping it is safe.
    b"Y": 20, b"L": 26, b"V": 35, b"W": 12, b"K": 28, b"J": 35,
    b"h": 21, b"B": 19, b"I": 50, b"N": 20,
}

# The seven types that mutate the book. Everything else is metadata or feeds the
# volume check ('P' and 'Q' are real volume that never touches the book).
BOOK_MUTATING = frozenset({b"A", b"F", b"E", b"C", b"X", b"D", b"U"})


def stock_locate(payload: bytes) -> int:
    """2-byte big-endian stock locate code, at offset 1."""
    return int.from_bytes(payload[1:3], "big")


def tracking_number(payload: bytes) -> int:
    """2-byte big-endian tracking number, at offset 3."""
    return int.from_bytes(payload[3:5], "big")


def read_timestamp(payload: bytes) -> int:
    """6-byte big-endian nanoseconds since midnight, at offset 5."""
    return int.from_bytes(payload[5:11], "big")


def _stock(payload: bytes, off: int) -> str:
    """8-char stock symbol, left-justified and space-padded on the wire."""
    return payload[off:off + 8].decode(errors="replace").rstrip()


def iter_messages(stream):
    """Yield (type_byte, payload) tuples from a binary ITCH stream.

    `stream` is any file-like object opened in binary mode (e.g. gzip.open).
    """
    while True:
        prefix = stream.read(2)
        if len(prefix) == 0:
            return  # clean EOF
        if len(prefix) != 2:
            raise ValueError("truncated length prefix")
        (length,) = struct.unpack(">H", prefix)
        payload = stream.read(length)
        if len(payload) != length:
            raise ValueError("truncated message body")
        mtype = payload[0:1]
        expected = SPEC_LENGTH.get(mtype)
        if expected is not None and length != expected:
            raise ValueError(f"length mismatch for {mtype!r}: {length} != {expected}")
        yield mtype, payload


# ---- per-type decoders -------------------------------------------------------
#
# Each returns a plain dict. Field comments give the offset and width so they can
# be checked against the spec's message-format table line by line.


def parse_system_event(p: bytes) -> dict:
    # event code @11 (1) — 'O' start of messages, 'S' start of system hours,
    # 'Q' market open, 'M' market close, 'E' end of system hours, 'C' end of day
    return {"code": p[11:12].decode()}


def parse_stock_directory(p: bytes) -> dict:
    # stock @11 (8) | market category @19 (1) | financial status @20 (1)
    # round lot size @21 (4)
    (round_lot,) = struct.unpack(">I", p[21:25])
    return {"stock": _stock(p, 11),
            "market_category": p[19:20].decode(),
            "financial_status": p[20:21].decode(),
            "round_lot_size": round_lot}


def parse_add_order(p: bytes) -> dict:
    # ref @11 (8) | side @19 (1) | shares @20 (4) | stock @24 (8) | price @32 (4)
    ref, side, shares = struct.unpack(">QcI", p[11:24])
    (price,) = struct.unpack(">I", p[32:36])
    return {"ref": ref, "side": side.decode(), "shares": shares,
            "stock": _stock(p, 24), "price": price}


def parse_add_order_mpid(p: bytes) -> dict:
    # identical to 'A' for the first 36 bytes, then attribution @36 (4)
    d = parse_add_order(p)
    d["attribution"] = p[36:40].decode(errors="replace").rstrip()
    return d


def parse_order_executed(p: bytes) -> dict:
    # ref @11 (8) | executed shares @19 (4) | match number @23 (8)
    ref, shares, match = struct.unpack(">QIQ", p[11:31])
    return {"ref": ref, "shares": shares, "match": match}


def parse_order_executed_price(p: bytes) -> dict:
    # ref @11 (8) | executed shares @19 (4) | match @23 (8)
    # printable @31 (1) | execution price @32 (4)
    ref, shares, match = struct.unpack(">QIQ", p[11:31])
    (price,) = struct.unpack(">I", p[32:36])
    return {"ref": ref, "shares": shares, "match": match,
            "printable": p[31:32] == b"Y", "price": price}


def parse_order_cancel(p: bytes) -> dict:
    # ref @11 (8) | cancelled shares @19 (4)
    ref, shares = struct.unpack(">QI", p[11:23])
    return {"ref": ref, "shares": shares}


def parse_order_delete(p: bytes) -> dict:
    # ref @11 (8)
    (ref,) = struct.unpack(">Q", p[11:19])
    return {"ref": ref}


def parse_order_replace(p: bytes) -> dict:
    # original ref @11 (8) | new ref @19 (8) | shares @27 (4) | price @31 (4)
    # Side and stock are inherited from the original order — they are not on the
    # wire. A replace loses queue priority: the new order goes to the back.
    orig, new, shares, price = struct.unpack(">QQII", p[11:35])
    return {"orig_ref": orig, "ref": new, "shares": shares, "price": price}


def parse_trade(p: bytes) -> dict:
    # 'P' non-cross trade — hidden liquidity, never touches the book.
    # ref @11 (8) | side @19 (1) | shares @20 (4) | stock @24 (8)
    # price @32 (4) | match @36 (8)
    ref, side, shares = struct.unpack(">QcI", p[11:24])
    price, match = struct.unpack(">IQ", p[32:44])
    return {"ref": ref, "side": side.decode(), "shares": shares,
            "stock": _stock(p, 24), "price": price, "match": match}


def parse_cross_trade(p: bytes) -> dict:
    # 'Q' opening/closing cross — never touches the book. Note the 8-byte share
    # count: crosses are far bigger than a single order can be.
    # shares @11 (8) | stock @19 (8) | cross price @27 (4) | match @31 (8)
    # cross type @39 (1) — 'O' opening, 'C' closing, 'H' halt/IPO, 'I' intraday
    (shares,) = struct.unpack(">Q", p[11:19])
    price, match = struct.unpack(">IQ", p[27:39])
    return {"shares": shares, "stock": _stock(p, 19), "price": price,
            "match": match, "cross_type": p[39:40].decode()}


def parse_trading_action(p: bytes) -> dict:
    # 'H' — stock trading action. Matters for phase 7.
    # stock @11 (8) | trading state @19 (1) | reserved @20 (1) | reason @21 (4)
    return {"stock": _stock(p, 11),
            "state": p[19:20].decode(),   # 'H' halted, 'P' paused, 'Q' quote-only, 'T' trading
            "reason": p[21:25].decode(errors="replace").rstrip()}


DECODERS = {
    b"S": parse_system_event,
    b"R": parse_stock_directory,
    b"A": parse_add_order,
    b"F": parse_add_order_mpid,
    b"E": parse_order_executed,
    b"C": parse_order_executed_price,
    b"X": parse_order_cancel,
    b"D": parse_order_delete,
    b"U": parse_order_replace,
    b"P": parse_trade,
    b"Q": parse_cross_trade,
    b"H": parse_trading_action,
}


def decode(mtype: bytes, payload: bytes):
    """Decode one message into a dict, or None for a type we don't model.

    The returned dict always carries 'type', 'locate' and 'ts'.
    """
    fn = DECODERS.get(mtype)
    if fn is None:
        return None
    d = fn(payload)
    d["type"] = mtype.decode()
    d["locate"] = stock_locate(payload)
    d["ts"] = read_timestamp(payload)
    return d
