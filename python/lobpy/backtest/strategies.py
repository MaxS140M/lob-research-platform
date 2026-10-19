"""Example strategies.

``BuyOpenSellClose`` exists to test the backtester: its result can be worked out by hand.
The two imbalance strategies trade the top-of-book imbalance signal from the notebook, one by
taking liquidity and one by providing it, to show what costs and queue position do to it.
"""

from __future__ import annotations

from .types import BUY, SELL, Account, Action, BookSnapshot, Cancel, Fill, LimitOrder, MarketOrder


class BuyOpenSellClose:
    """Buys ``quantity`` at market on the first event at or after ``open_ns``.

    The backtester's forced flat at ``flatten_at_ns`` sells it again.
    """

    def __init__(self, quantity: int = 100, open_ns: int = 0) -> None:
        self.quantity = quantity
        self.open_ns = open_ns
        self.sent = False

    def on_book_update(self, book: BookSnapshot) -> list[Action] | None:
        if not self.sent and book.timestamp >= self.open_ns and book.ask_price:
            self.sent = True
            return [MarketOrder(BUY, self.quantity)]
        return None

    def on_fill(self, fill: Fill) -> None:
        pass


def _ioc_at_touch(book: BookSnapshot, side: int, quantity: int) -> LimitOrder | None:
    """Immediate-or-cancel at the best opposite price: takes only what the touch offers."""
    price = book.ask_price if side == BUY else book.bid_price
    return LimitOrder(side, quantity, price, ioc=True) if price else None


class _HoldThenExit:
    """Shared exit logic: once in a position, hold for ``hold_ns``, then send IOCs at the
    touch on each event until flat (a thin book may need several)."""

    hold_ns: int

    def _manage_position(self, book: BookSnapshot) -> list[Action] | None:
        a = self.account
        if not a.position:
            self.entered_at = None
            self.exiting = False
            return None
        if self.entered_at is None:
            self.entered_at = book.timestamp
        if self.exiting or book.timestamp - self.entered_at >= self.hold_ns:
            self.exiting = True
            order = _ioc_at_touch(book, SELL if a.position > 0 else BUY, abs(a.position))
            return [order] if order else None
        return None


class ImbalanceTaker(_HoldThenExit):
    """Crosses the spread at the touch (IOC) in the direction of a strong top-of-book
    imbalance, holds for ``hold_ns``, then exits at the touch. One position at a time."""

    def __init__(
        self,
        threshold: float = 0.8,
        quantity: int = 100,
        hold_ns: int = 10**10,
        cooldown_ns: int = 10**9,
    ) -> None:
        self.threshold = threshold
        self.quantity = quantity
        self.hold_ns = hold_ns
        self.cooldown_ns = cooldown_ns
        self.account: Account | None = None
        self.entered_at: int | None = None
        self.exiting = False
        self.next_entry = 0

    def on_start(self, account: Account) -> None:
        self.account = account

    def on_book_update(self, book: BookSnapshot) -> list[Action] | None:
        if self.account.open_orders:
            return None  # wait for in-flight orders to resolve
        was_exiting = self.exiting
        actions = self._manage_position(book)
        if self.account.position:
            return actions
        if was_exiting:
            self.next_entry = book.timestamp + self.cooldown_ns
        if book.timestamp < self.next_entry:
            return None
        imbalance = book.imbalance
        if imbalance is None or abs(imbalance) < self.threshold:
            return None
        order = _ioc_at_touch(book, BUY if imbalance > 0 else SELL, self.quantity)
        return [order] if order else None

    def on_fill(self, fill: Fill) -> None:
        pass


class ImbalanceMaker(_HoldThenExit):
    """Joins the queue at the best price on the side a strong imbalance favours, cancels if
    not filled within ``patience_ns``, holds for ``hold_ns``, then exits at the touch (IOC).

    Passive entry earns the spread instead of paying it, but only fills when someone trades
    against it, which tends to be when the price is about to move the wrong way (adverse
    selection).
    """

    def __init__(
        self,
        threshold: float = 0.8,
        quantity: int = 100,
        hold_ns: int = 10**10,
        patience_ns: int = 2 * 10**9,
    ) -> None:
        self.threshold = threshold
        self.quantity = quantity
        self.hold_ns = hold_ns
        self.patience_ns = patience_ns
        self.account: Account | None = None
        self.working: LimitOrder | None = None
        self.sent_at = 0
        self.cancel_sent = False
        self.entered_at: int | None = None
        self.exiting = False

    def on_start(self, account: Account) -> None:
        self.account = account

    def on_book_update(self, book: BookSnapshot) -> list[Action] | None:
        a = self.account
        t = book.timestamp
        if self.working is not None:
            if self.working.id not in a.open_orders:
                self.working, self.cancel_sent = None, False  # filled or cancelled
            elif not self.cancel_sent and t - self.sent_at >= self.patience_ns:
                self.cancel_sent = True
                return [Cancel(self.working.id)]
            else:
                return None
        if a.open_orders:
            return None
        actions = self._manage_position(book)
        if a.position:
            return actions
        imbalance = book.imbalance
        if imbalance is None or abs(imbalance) < self.threshold:
            return None
        side = BUY if imbalance > 0 else SELL
        self.working = LimitOrder(
            side, self.quantity, book.bid_price if side == BUY else book.ask_price
        )
        self.sent_at = t
        return [self.working]

    def on_fill(self, fill: Fill) -> None:
        pass
