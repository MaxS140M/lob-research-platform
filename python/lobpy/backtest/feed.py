"""The data feed: one symbol's book events, in time order, from the C++ engine."""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from typing import TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    from .._lob import Replayer

COLUMNS = (
    "timestamp",
    "seq",
    "kind",
    "side",
    "price",
    "shares",
    "trade_price",
    "priority",
    "old_price",
    "old_shares",
    "old_priority",
)
LEVELS = ("bid_price", "bid_size", "ask_price", "ask_size")


class EventFeed:
    """Column arrays of book events (see ``lobpy.Replayer.events``), iterated in chunks.

    The backtester walks the events in Python, so each chunk of columns is converted to
    lists once: plain Python ints are much faster to read in a loop than NumPy scalars. The
    depth arrays stay in NumPy and are only read when an order needs more than the top level.
    """

    def __init__(self, columns: Mapping[str, np.ndarray], symbol: str = "") -> None:
        missing = [c for c in (*COLUMNS, *LEVELS) if c not in columns]
        if missing:
            raise ValueError(f"event columns missing: {missing}")
        self.symbol = symbol
        self.columns = {c: np.asarray(columns[c]) for c in (*COLUMNS, *LEVELS)}
        n = len(self.columns["timestamp"])
        if any(len(a) != n for a in self.columns.values()):
            raise ValueError("event columns have different lengths")
        if n and np.any(np.diff(self.columns["timestamp"]) < 0):
            raise ValueError("events are not in time order")
        self.depth = self.columns["bid_price"].shape[1] if n else 0

    @classmethod
    def from_replayer(
        cls,
        replayer: Replayer,
        symbol: str,
        depth: int = 5,
        start_ns: int | None = None,
        end_ns: int | None = None,
    ) -> EventFeed:
        return cls(replayer.events(symbol, depth, start_ns=start_ns, end_ns=end_ns), symbol)

    def __len__(self) -> int:
        return len(self.columns["timestamp"])

    def chunks(self, size: int = 65_536) -> Iterator[tuple[int, dict[str, list]]]:
        """Yields (first row index, {column: list}) with top-of-book columns included."""
        c = self.columns
        for start in range(0, len(self), size):
            stop = min(start + size, len(self))
            out = {name: c[name][start:stop].tolist() for name in COLUMNS}
            for name in LEVELS:
                out[name + "_1"] = c[name][start:stop, 0].tolist()
            yield start, out

    # ---- Depth access, used by BookSnapshot.levels and the execution simulator ----

    def bids(self, row: int) -> tuple[list[int], list[int]]:
        if row < 0:
            return [], []
        return self.columns["bid_price"][row].tolist(), self.columns["bid_size"][row].tolist()

    def asks(self, row: int) -> tuple[list[int], list[int]]:
        if row < 0:
            return [], []
        return self.columns["ask_price"][row].tolist(), self.columns["ask_size"][row].tolist()
