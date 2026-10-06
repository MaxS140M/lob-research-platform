"""Orders, fills, book snapshots, the strategy interface and the backtest configuration.

All prices are integers with 4 implied decimals, exactly as in the C++ engine, so the
accounting is exact. ``lobpy.PRICE_SCALE`` (10,000) converts them to dollars.
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import Protocol, runtime_checkable

BUY = 1
SELL = -1

SECOND = 1_000_000_000
MINUTE = 60 * SECOND
HOUR = 60 * MINUTE


# ---- Orders a strategy can send --------------------------------------------------------------


@dataclass(slots=True)
class MarketOrder:
    """Takes liquidity at any price; any part the visible book cannot fill is cancelled."""

    side: int  # BUY or SELL
    quantity: int
    id: int = -1  # assigned by the backtester on submission


@dataclass(slots=True)
class LimitOrder:
    """Trades up to ``price``; the rest joins the back of the queue at that price.

    With ``ioc=True`` (immediate-or-cancel) the rest is cancelled instead of resting. An IOC
    at the touch is how a careful trader takes liquidity: it can never walk down a thin book.
    """

    side: int
    quantity: int
    price: int
    ioc: bool = False
    id: int = -1


@dataclass(frozen=True, slots=True)
class Cancel:
    """Cancels whatever is left of an order. Subject to the same latency as new orders."""

    order_id: int


Order = MarketOrder | LimitOrder
Action = MarketOrder | LimitOrder | Cancel


@dataclass(frozen=True, slots=True)
class Fill:
    order_id: int
    timestamp: int  # ns since midnight
    side: int
    price: int
    quantity: int
    fee: int  # in price units (1e-4 $); negative would be a rebate
    liquidity: str  # "taker" or "maker"
    mid: float  # mid price (price units) when the fill happened, for the PnL breakdown


# ---- What a strategy sees ------------------------------------------------------------------


class BookSnapshot:
    """The book right after one event. Strategies only ever receive the current snapshot.

    Top of book is in ``bid_price`` / ``bid_size`` / ``ask_price`` / ``ask_size`` (0 when a
    side is empty). ``kind`` is the ITCH type of the event that produced it. For executions
    ('E', 'C', 'P'), ``trade_price`` / ``trade_shares`` / ``trade_side`` describe the trade,
    with ``trade_side`` the aggressor (+1 buyer-initiated, -1 seller-initiated).
    """

    __slots__ = (
        "timestamp", "kind", "bid_price", "bid_size", "ask_price", "ask_size",
        "trade_price", "trade_shares", "trade_side", "_levels", "_row",
    )  # fmt: skip

    def __init__(self, timestamp, kind, bid_price, bid_size, ask_price, ask_size,
                 trade_price=0, trade_shares=0, trade_side=0, levels=None, row=-1):  # fmt: skip
        self.timestamp = timestamp
        self.kind = kind
        self.bid_price = bid_price
        self.bid_size = bid_size
        self.ask_price = ask_price
        self.ask_size = ask_size
        self.trade_price = trade_price
        self.trade_shares = trade_shares
        self.trade_side = trade_side
        self._levels = levels
        self._row = row

    @property
    def mid(self) -> float | None:
        if self.bid_price and self.ask_price:
            return (self.bid_price + self.ask_price) / 2
        return None

    @property
    def spread(self) -> int | None:
        if self.bid_price and self.ask_price:
            return self.ask_price - self.bid_price
        return None

    @property
    def imbalance(self) -> float | None:
        """(bid size - ask size) / (bid size + ask size) at the top of book, in [-1, 1]."""
        total = self.bid_size + self.ask_size
        if not (self.bid_size and self.ask_size):
            return None
        return (self.bid_size - self.ask_size) / total

    def levels(self, side: int) -> list[tuple[int, int]]:
        """[(price, size), ...] best first for BUY (bids) or SELL (asks), up to the feed depth."""
        if self._levels is None:
            return []
        prices, sizes = self._levels.bids(self._row) if side == BUY else self._levels.asks(self._row)
        return [(p, s) for p, s in zip(prices, sizes) if s > 0]


@dataclass
class Account:
    """The strategy's own state, kept up to date by the backtester. Read it, don't write it."""

    position: int = 0
    cash: int = 0  # price units; excludes fees
    fees: int = 0
    open_orders: dict[int, Order] = field(default_factory=dict)  # sent and not yet done


@runtime_checkable
class Strategy(Protocol):
    """Receives one event at a time and may respond with orders and cancels.

    A strategy never gets direct access to the feed, so it cannot look ahead. It may also
    define ``on_start(account)`` to receive its live Account.
    """

    def on_book_update(self, book: BookSnapshot) -> Iterable[Action] | None: ...

    def on_fill(self, fill: Fill) -> None: ...


# ---- Configuration -------------------------------------------------------------------------


@dataclass(frozen=True)
class BacktestConfig:
    """Assumptions of a run. Every number here is reported alongside the results."""

    latency_ns: int = 100_000  # one-way: an order sent at t is live at the exchange at t + this
    taker_fee: float = 0.0030  # $ per share when taking liquidity
    maker_fee: float = 0.0030  # $ per share when providing liquidity (negative = rebate)
    max_position: int = 1_000  # absolute shares, counting open orders in the worst case
    flatten_at_ns: int | None = 15 * HOUR + 55 * MINUTE  # cancel all and go flat from here on
    depth: int = 5  # book levels available to market orders and queue estimates

    def fee_units(self, liquidity: str) -> int:
        """Per-share fee in price units (1e-4 $)."""
        fee = self.taker_fee if liquidity == "taker" else self.maker_fee
        return round(fee * 10_000)

