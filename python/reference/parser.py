"""Slow, obvious ITCH 5.0 parser — the oracle (Phase 2).

No cleverness. struct.unpack with big-endian format strings. This exists to be
correct, not fast: it is the ground truth the C++ implementation is diffed
against for the next six months.
"""
import struct

# payload length (including type byte) for the types we model
SPEC_LENGTH = {
    b"S": 12, b"R": 39, b"A": 36, b"F": 40, b"E": 31,
    b"C": 36, b"X": 23, b"D": 19, b"U": 35, b"P": 44, b"Q": 40, b"H": 25,
}


def read_timestamp(payload: bytes) -> int:
    """6-byte big-endian nanoseconds since midnight, at offset 5."""
    return int.from_bytes(payload[5:11], "big")


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


def parse_add_order(p: bytes) -> dict:
    # ref @11 (8) | side @19 (1) | shares @20 (4) | stock @24 (8) | price @32 (4)
    ref, side, shares = struct.unpack(">QcI", p[11:24])
    stock = p[24:32].decode(errors="replace").strip()
    (price,) = struct.unpack(">I", p[32:36])
    return {"ref": ref, "side": side.decode(), "shares": shares,
            "stock": stock, "price": price}
