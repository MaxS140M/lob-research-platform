"""The execution simulator: decides if, when and at what price the strategy's orders fill.

The strategy's orders are *virtual*: they never enter the replayed book, so they have no market
impact. Everything else is modelled conservatively:

- **Latency.** Orders and cancels sent at time t reach the exchange at t + latency. They are
  then handled against the book as it stood at that moment: the state after the last event
  at or before the arrival time.
- **Taking liquidity.** A market order, or the marketable part of a limit order, walks the
  opposite side of the visible book: buys pay the asks, sells receive the bids, level by
  level. Whatever the visible depth cannot fill is cancelled (market and IOC) or rests
  (limit).
- **Queue position.** A resting limit order joins the *back* of the queue at its price. It
  starts with ``ahead`` = the displayed size at that price when it arrived. Only events on
  orders that were already there when it arrived (queue priority <= its join seq) reduce
  ``ahead``: executions, partial cancels, deletes, and the removal half of a replace.
- **Passive fills.** The order fills when:
  - an order *behind* it at its price executes (FIFO means the aggressor would have reached
    it first);
  - a hidden-order trade at its price happens while nothing is ahead;
  - an execution happens at a price *worse* than its own (the market traded through it);
  - the opposite side's best price reaches its price.

  Passive fills are always at the order's own limit price.
- **Fees** are charged per share, separately for taking and providing liquidity.

Known simplifications, chosen to err against the strategy:

- Hidden orders are assumed to have no queue priority over the strategy's displayed order.
- Prices deeper than the feed's visible depth have unknown queue size, so an order resting
  there only fills on a trade-through or a cross.
- Opening and closing crosses ('Q') never fill the strategy's orders.
"""

from __future__ import annotations

import math
from collections import deque
from collections.abc import Callable

from .feed import EventFeed
from .types import BUY, SELL, BacktestConfig, Cancel, Fill, LimitOrder, MarketOrder, Order

UNKNOWN_QUEUE = math.inf


class _Working:
    """A resting order at the exchange."""

    __slots__ = ("order", "side", "price", "remaining", "ahead", "join_seq")

    def __init__(self, order: LimitOrder, remaining: int, ahead: float, join_seq: int):
        self.order = order
        self.side = order.side
        self.price = order.price
        self.remaining = remaining
        self.ahead = ahead
        self.join_seq = join_seq


class ExecutionSimulator:
    def __init__(self, config: BacktestConfig, feed: EventFeed,
                 on_fill: Callable[[Fill], None], on_done: Callable[[int], None]):  # fmt: skip
        self.config = config
        self.feed = feed
        self._on_fill = on_fill
        self._on_done = on_done
        self._maker_fee = config.fee_units("maker")
        self._taker_fee = config.fee_units("taker")

        self.in_flight: deque[tuple[int, MarketOrder | LimitOrder | Cancel]] = deque()
        self.resting: dict[int, _Working] = {}
        self.open_quantity = {BUY: 0, SELL: 0}  # in flight + resting, for risk checks

        self.submitted_shares = 0
        self.filled_shares = 0
        self.cancelled_shares = 0

    # ---- Messages from the strategy ----

    def send(self, action: Order | Cancel, now: int) -> None:
        self.in_flight.append((now + self.config.latency_ns, action))
        if not isinstance(action, Cancel):
            self.open_quantity[action.side] += action.quantity
            self.submitted_shares += action.quantity

    def deliver_due(self, before: int, row: int, seq: int, mid: float) -> None:
        """Delivers every message that reached the exchange strictly before ``before``."""
        while self.in_flight and self.in_flight[0][0] < before:
            arrival, action = self.in_flight.popleft()
            self._arrive(action, arrival, row, seq, mid)

    def deliver_all(self, row: int, seq: int, mid: float) -> None:
        """At the end of the data: everything still in flight arrives against the last book."""
        while self.in_flight:
            arrival, action = self.in_flight.popleft()
            self._arrive(action, arrival, row, seq, mid)

    # ---- Arrival at the exchange ----

    def _arrive(self, action, t: int, row: int, seq: int, mid: float) -> None:
        if isinstance(action, Cancel):
            w = self.resting.pop(action.order_id, None)
            if w is not None:
                self._close(w.order, w.remaining)
            return

        order = action
        limit = order.price if isinstance(order, LimitOrder) else None
        remaining = self._take(order, limit, order.quantity, row, t, mid)
        if remaining == 0:
            self._on_done(order.id)
        elif limit is None or order.ioc:
            self._close(order, remaining)  # market or IOC: never rests
        else:
            ahead = self._queue_ahead(row, order.side, order.price)
            self.resting[order.id] = _Working(order, remaining, ahead, seq)

    def _take(self, order: Order, limit: int | None, quantity: int, row: int, t: int,
              mid: float) -> int:  # fmt: skip
        """Fills against the opposite side of the visible book; returns what is left."""
        prices, sizes = self.feed.asks(row) if order.side == BUY else self.feed.bids(row)
        remaining = quantity
        for price, size in zip(prices, sizes):
            if size <= 0 or remaining == 0:
                break
            if limit is not None and (price > limit if order.side == BUY else price < limit):
                break
            q = min(size, remaining)
            self._fill(order, t, price, q, "taker", mid)
            remaining -= q
        return remaining

    def _queue_ahead(self, row: int, side: int, price: int) -> float:
        """Displayed size already waiting at ``price`` on ``side`` when the order arrives."""
        prices, sizes = self.feed.bids(row) if side == BUY else self.feed.asks(row)
        levels = [(p, s) for p, s in zip(prices, sizes) if s > 0]
        for p, s in levels:
            if p == price:
                return s
        if not levels:
            return 0
        best, worst = levels[0][0], levels[-1][0]
        if (price > best) if side == BUY else (price < best):
            return 0  # a new best price: first in its queue
        if len(levels) < self.feed.depth or ((price > worst) if side == BUY else (price < worst)):
            return 0  # inside the visible range, and nothing is resting there
        return UNKNOWN_QUEUE  # deeper than the feed shows: assume we never reach the front

    # ---- Book events while orders rest ----

    def on_event(self, kind: int, side: int, price: int, shares: int, priority: int,
                 old_price: int, old_shares: int, old_priority: int, bid: int, ask: int,
                 t: int, mid: float) -> None:  # fmt: skip
        """Updates queue positions and passive fills for one book event (kind = ITCH byte)."""
        for w in list(self.resting.values()):
            if side == w.side:
                self._same_side_event(w, kind, price, shares, priority, old_price, old_shares,
                                      old_priority, t, mid)  # fmt: skip
            if w.remaining == 0:
                continue
            # The opposite side reached our price: an incoming order there would have hit us.
            crossed = (ask and ask <= w.price) if w.side == BUY else (bid and bid >= w.price)
            if crossed:
                self._passive_fill(w, w.remaining, t, mid)

    def _same_side_event(self, w: _Working, kind: int, price: int, shares: int, priority: int,
                         old_price: int, old_shares: int, old_priority: int, t: int,
                         mid: float) -> None:  # fmt: skip
        worse = price < w.price if w.side == BUY else price > w.price
        if kind == 69 or kind == 67:  # 'E', 'C': a resting order on our side executed
            if price == w.price:
                if priority <= w.join_seq:
                    w.ahead = max(0, w.ahead - shares)
                else:  # an order behind us traded, so the aggressor reached us first
                    w.ahead = 0
                    self._passive_fill(w, min(shares, w.remaining), t, mid)
            elif worse:
                self._passive_fill(w, w.remaining, t, mid)  # traded through our price
        elif kind == 80:  # 'P': a hidden order on our side traded
            if price == w.price and w.ahead == 0:
                self._passive_fill(w, min(shares, w.remaining), t, mid)
            elif worse:
                self._passive_fill(w, w.remaining, t, mid)
        elif kind == 88 or kind == 68:  # 'X', 'D': shares left the queue without trading
            if price == w.price and priority <= w.join_seq:
                w.ahead = max(0, w.ahead - shares)
        elif kind == 85:  # 'U': the replaced order leaves its old place
            if old_price == w.price and old_priority <= w.join_seq:
                w.ahead = max(0, w.ahead - old_shares)

    # ---- Bookkeeping ----

    def _passive_fill(self, w: _Working, quantity: int, t: int, mid: float) -> None:
        if quantity <= 0:
            return
        self._fill(w.order, t, w.price, quantity, "maker", mid)
        w.remaining -= quantity
        if w.remaining == 0:
            del self.resting[w.order.id]
            self._on_done(w.order.id)

    def _fill(self, order: Order, t: int, price: int, quantity: int, liquidity: str,
              mid: float) -> None:  # fmt: skip
        fee = (self._taker_fee if liquidity == "taker" else self._maker_fee) * quantity
        self.open_quantity[order.side] -= quantity
        self.filled_shares += quantity
        self._on_fill(Fill(order.id, t, order.side, price, quantity, fee, liquidity, mid))

    def _close(self, order: Order, remaining: int) -> None:
        """The rest of an order is cancelled (by request, or a market order ran out of book)."""
        self.open_quantity[order.side] -= remaining
        self.cancelled_shares += remaining
        self._on_done(order.id)


__all__ = ["ExecutionSimulator", "UNKNOWN_QUEUE"]
