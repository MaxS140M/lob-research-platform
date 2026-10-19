"""Trading a model's predictions in the backtester."""

from __future__ import annotations

import numpy as np

from ..backtest.strategies import _HoldThenExit, _ioc_at_touch
from ..backtest.types import BUY, SELL, Account, Action, BookSnapshot, Fill


class PredictionTaker(_HoldThenExit):
    """Buys when the predicted probability of an up move exceeds ``threshold`` (sells for
    down), crossing at the touch with an IOC. Holds for ``hold_ns`` (the label horizon), then
    exits at the touch.

    Predictions are made on a time grid: the one stamped *t* uses only the book at or before
    *t*. At an event at time *e*, the strategy uses the latest prediction with *t* <= *e*,
    so it never sees a prediction built from later data.

    With ``max_spread`` (price units) it only enters when the current spread is at most that
    wide, so it never pays a wide spread to act on a prediction.
    """

    def __init__(
        self,
        timestamps: np.ndarray,
        p_up: np.ndarray,
        p_down: np.ndarray,
        threshold: float = 0.6,
        quantity: int = 100,
        hold_ns: int = 10**9,
        max_spread: int | None = None,
    ) -> None:
        self.timestamps = np.asarray(timestamps)
        self.p_up = np.asarray(p_up)
        self.p_down = np.asarray(p_down)
        self.threshold = threshold
        self.quantity = quantity
        self.hold_ns = hold_ns
        self.max_spread = max_spread
        self.account: Account | None = None
        self.entered_at: int | None = None
        self.exiting = False
        self.signals = 0

    def on_start(self, account: Account) -> None:
        self.account = account

    def on_book_update(self, book: BookSnapshot) -> list[Action] | None:
        if self.account.open_orders:
            return None
        actions = self._manage_position(book)
        if self.account.position:
            return actions
        if self.max_spread is not None and (book.spread is None or book.spread > self.max_spread):
            return None
        i = np.searchsorted(self.timestamps, book.timestamp, side="right") - 1
        if i < 0:
            return None
        if self.p_up[i] > self.threshold:
            side = BUY
        elif self.p_down[i] > self.threshold:
            side = SELL
        else:
            return None
        self.signals += 1
        order = _ioc_at_touch(book, side, self.quantity)
        return [order] if order else None

    def on_fill(self, fill: Fill) -> None:
        pass
