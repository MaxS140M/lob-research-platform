"""The portfolio: position, cash, fees, and the risk limit."""

from __future__ import annotations

from .types import BUY, Account, BacktestConfig, Fill, Order


class Portfolio:
    """Tracks the account from fills, and checks new orders against the position limit.

    Cash and fees are integers in price units (1e-4 $), so PnL is exact. ``spread_pnl``
    accumulates, for each fill, how far the price was from the mid at that moment:
    (mid - price) x shares for buys, (price - mid) x shares for sells. That is what the
    strategy paid to cross the spread (negative) or earned by providing liquidity (positive).
    """

    def __init__(self, config: BacktestConfig):
        self.config = config
        self.account = Account()
        self.spread_pnl = 0.0
        self.traded_shares = 0
        self.max_abs_position = 0
        self.fills: list[Fill] = []

    @property
    def position(self) -> int:
        return self.account.position

    def apply(self, fill: Fill) -> None:
        a = self.account
        a.position += fill.side * fill.quantity
        a.cash -= fill.side * fill.price * fill.quantity
        a.fees += fill.fee
        self.spread_pnl += fill.side * (fill.mid - fill.price) * fill.quantity
        self.traded_shares += fill.quantity
        self.max_abs_position = max(self.max_abs_position, abs(a.position))
        self.fills.append(fill)

    def allows(self, order: Order, open_quantity: dict[int, int]) -> bool:
        """True if the position stays within the limit even if every open order fills."""
        if order.quantity <= 0:
            return False
        if order.side == BUY:
            worst = self.account.position + open_quantity[BUY] + order.quantity
        else:
            worst = -(self.account.position - open_quantity[-1] - order.quantity)
        return worst <= self.config.max_position

    def equity(self, mid: float) -> float:
        """Mark-to-market value after fees, in price units."""
        a = self.account
        return a.cash + a.position * mid - a.fees
