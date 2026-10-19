"""Write testdata/sample.itch: a tiny hand-built ITCH 5.0 day used by the C++ and Python tests.

Two symbols, AAA (locate 1) and BBB (locate 2), with one of each book-changing message and each
trade type. The expected books and trades are listed in testdata/README.md and asserted by
cpp/tests/test_replayer.cpp and python/tests/test_replayer.py.

usage: python3 scripts/make_sample_itch.py
"""

from __future__ import annotations

import struct
from pathlib import Path

SECOND = 1_000_000_000


def ts48(ns: int) -> bytes:
    return ns.to_bytes(6, "big")


def header(kind: str, locate: int, ns: int) -> bytes:
    return kind.encode() + struct.pack(">HH", locate, 0) + ts48(ns)


def alpha(text: str, width: int) -> bytes:
    return text.ljust(width).encode()


def system_event(ns: int, code: str) -> bytes:
    return header("S", 0, ns) + code.encode()


def stock_directory(ns: int, locate: int, symbol: str) -> bytes:
    return (
        header("R", locate, ns)
        + alpha(symbol, 8)
        + b"Q"  # market category
        + b"N"  # financial status
        + struct.pack(">I", 100)  # round lot
        + b"N"  # round lots only
        + b"C"  # issue classification
        + alpha("Z", 2)  # issue sub-type
        + b"P"  # authenticity
        + b"N"  # short sale threshold
        + b" "  # IPO flag
        + b"1"  # LULD tier
        + b"N"  # ETP flag
        + struct.pack(">I", 0)  # ETP leverage
        + b"N"  # inverse
    )


def add(ns: int, locate: int, ref: int, side: str, shares: int, symbol: str, price: int) -> bytes:
    return (
        header("A", locate, ns)
        + struct.pack(">Q", ref)
        + side.encode()
        + struct.pack(">I", shares)
        + alpha(symbol, 8)
        + struct.pack(">I", price)
    )


def add_mpid(ns, locate, ref, side, shares, symbol, price, mpid) -> bytes:
    return b"F" + add(ns, locate, ref, side, shares, symbol, price)[1:] + alpha(mpid, 4)


def executed(ns: int, locate: int, ref: int, shares: int, match: int) -> bytes:
    return header("E", locate, ns) + struct.pack(">QIQ", ref, shares, match)


def executed_with_price(ns, locate, ref, shares, match, printable, price) -> bytes:
    return (
        header("C", locate, ns)
        + struct.pack(">QIQ", ref, shares, match)
        + printable.encode()
        + struct.pack(">I", price)
    )


def cancel(ns: int, locate: int, ref: int, shares: int) -> bytes:
    return header("X", locate, ns) + struct.pack(">QI", ref, shares)


def delete(ns: int, locate: int, ref: int) -> bytes:
    return header("D", locate, ns) + struct.pack(">Q", ref)


def replace(ns: int, locate: int, old: int, new: int, shares: int, price: int) -> bytes:
    return header("U", locate, ns) + struct.pack(">QQII", old, new, shares, price)


def trade(ns, locate, side, shares, symbol, price, match) -> bytes:
    return (
        header("P", locate, ns)
        + struct.pack(">Q", 0)
        + side.encode()
        + struct.pack(">I", shares)
        + alpha(symbol, 8)
        + struct.pack(">IQ", price, match)
    )


def cross(ns, locate, shares, symbol, price, match, cross_type) -> bytes:
    return (
        header("Q", locate, ns)
        + struct.pack(">Q", shares)
        + alpha(symbol, 8)
        + struct.pack(">IQ", price, match)
        + cross_type.encode()
    )


SIZES = {
    "S": 12,
    "R": 39,
    "A": 36,
    "F": 40,
    "E": 31,
    "C": 36,
    "X": 23,
    "D": 19,
    "U": 35,
    "P": 44,
    "Q": 40,
}


def messages() -> list[bytes]:
    s = SECOND
    return [
        system_event(1 * s, "O"),
        stock_directory(1 * s, 1, "AAA"),
        stock_directory(1 * s, 2, "BBB"),
        add(10 * s, 1, 1, "B", 100, "AAA", 100_000),  # AAA bid $10.00 x 100
        add(10 * s + s // 10, 1, 2, "B", 200, "AAA", 99_900),  # AAA bid $9.99 x 200
        add(10 * s + 2 * s // 10, 1, 3, "S", 150, "AAA", 100_100),  # AAA ask $10.01 x 150
        add(10 * s + 3 * s // 10, 1, 4, "S", 50, "AAA", 100_200),  # AAA ask $10.02 x 50
        add_mpid(10 * s + 4 * s // 10, 2, 5, "S", 300, "BBB", 500_000, "GSCO"),  # BBB ask $50.00
        add(10 * s + 5 * s // 10, 2, 6, "B", 100, "BBB", 499_500),  # BBB bid $49.95 x 100
        executed(11 * s, 1, 3, 50, 1001),  # buyer lifts 50 of the $10.01 offer
        cancel(11 * s + s // 2, 1, 2, 50),  # $9.99 bid 200 -> 150
        executed_with_price(12 * s, 1, 1, 100, 1002, "Y", 99_950),  # $10.00 bid filled at $9.995
        replace(12 * s + s // 2, 1, 4, 7, 80, 100_150),  # $10.02 x 50 -> $10.015 x 80
        delete(13 * s, 2, 6),  # BBB bid gone
        trade(13 * s + s // 2, 2, "S", 25, "BBB", 499_800, 1003),  # hidden sell hit by a buyer
        cross(14 * s, 1, 1_000, "AAA", 100_000, 1004, "C"),  # closing cross
        system_event(15 * s, "C"),
    ]


def main() -> None:
    out = Path(__file__).resolve().parents[1] / "testdata" / "sample.itch"
    out.parent.mkdir(exist_ok=True)
    data = bytearray()
    for msg in messages():
        assert len(msg) == SIZES[chr(msg[0])], (chr(msg[0]), len(msg))
        data += struct.pack(">H", len(msg)) + msg
    out.write_bytes(bytes(data))
    print(f"wrote {out} ({len(data)} bytes, {len(messages())} messages)")


if __name__ == "__main__":
    main()
