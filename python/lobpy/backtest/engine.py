"""The event loop that connects feed, strategy, execution simulator and portfolio.

For each event, in time order:

1. Orders and cancels whose latency has elapsed reach the exchange, and are handled against
   the book as it stood *before* this event.
2. The event updates the book. Resting orders update their queue position and may fill.
3. From ``flatten_at_ns`` on, the backtester takes over: it cancels everything, then sends
   immediate-or-cancel orders at the touch, event after event, until the position is flat.
   (A market order could sweep a thin book far from the touch.)
4. Otherwise the strategy sees the new book, and nothing else, and may send orders.
"""

from __future__ import annotations

from .execution import ExecutionSimulator
from .feed import EventFeed
from .metrics import BacktestResult, build_result
from .portfolio import Portfolio
from .types import (
    BUY,
    SECOND,
    SELL,
    BacktestConfig,
    BookSnapshot,
    Cancel,
    Fill,
    LimitOrder,
    MarketOrder,
    Strategy,
)

_E, _C, _P = ord("E"), ord("C"), ord("P")


class Backtester:
    def __init__(self, feed: EventFeed, config: BacktestConfig | None = None):
        self.feed = feed
        self.config = config or BacktestConfig()
        if self.feed.depth and self.config.depth > self.feed.depth:
            raise ValueError(f"config.depth {self.config.depth} exceeds feed depth {self.feed.depth}")

    def run(self, strategy: Strategy) -> BacktestResult:
        cfg = self.config
        portfolio = Portfolio(cfg)
        account = portfolio.account
        next_id = 0
        orders = rejected = 0

        def on_fill(fill: Fill) -> None:
            portfolio.apply(fill)
            strategy.on_fill(fill)

        def on_done(order_id: int) -> None:
            account.open_orders.pop(order_id, None)

        sim = ExecutionSimulator(cfg, self.feed, on_fill, on_done)

        def submit(action, now: int, *, bypass_risk: bool = False) -> None:
            nonlocal next_id, orders, rejected
            if isinstance(action, Cancel):
                sim.send(action, now)
                return
            if not isinstance(action, (MarketOrder, LimitOrder)):
                raise TypeError(f"strategy returned {action!r}, expected an order or Cancel")
            orders += 1
            if isinstance(action, LimitOrder) and action.price <= 0:
                rejected += 1
                return
            if not bypass_risk and not portfolio.allows(action, sim.open_quantity):
                rejected += 1
                return
            next_id += 1
            action.id = next_id
            account.open_orders[next_id] = action
            sim.send(action, now)

        if hasattr(strategy, "on_start"):
            strategy.on_start(account)

        flatten_at = cfg.flatten_at_ns
        flattening = False
        flatten_order: int | None = None
        mid = 0.0  # last valid mid, in price units
        prev_row, prev_seq, prev_mid = -1, 0, 0.0
        equity_samples: list[tuple[int, float]] = []
        next_sample = 0

        for start, c in self.feed.chunks():
            ts, seq, kind, side = c["timestamp"], c["seq"], c["kind"], c["side"]
            price, shares, trade_price, priority = c["price"], c["shares"], c["trade_price"], c["priority"]
            old_price, old_shares, old_priority = c["old_price"], c["old_shares"], c["old_priority"]
            bid_px, bid_sz, ask_px, ask_sz = c["bid_price_1"], c["bid_size_1"], c["ask_price_1"], c["ask_size_1"]

            for j in range(len(ts)):
                t = ts[j]
                # 1. Messages that reached the exchange before this event.
                if sim.in_flight and sim.in_flight[0][0] < t:
                    sim.deliver_due(t, prev_row, prev_seq, prev_mid)

                # 2. Apply the event.
                bid, ask = bid_px[j], ask_px[j]
                if bid and ask:
                    mid = (bid + ask) / 2
                if sim.resting:
                    sim.on_event(kind[j], side[j], price[j], shares[j], priority[j], old_price[j],
                                 old_shares[j], old_priority[j], bid, ask, t, mid)  # fmt: skip
                prev_row, prev_seq, prev_mid = start + j, seq[j], mid

                if t >= next_sample:
                    equity_samples.append((t, portfolio.equity(mid)))
                    next_sample = (t // SECOND + 1) * SECOND

                # 3. Forced flat before the close.
                if flatten_at is not None and t >= flatten_at:
                    if not flattening:
                        flattening = True
                        for order_id in list(account.open_orders):
                            sim.send(Cancel(order_id), t)
                    pos = account.position
                    touch = bid if pos > 0 else ask
                    if pos and touch and (flatten_order is None
                                          or flatten_order not in account.open_orders):
                        side_out = SELL if pos > 0 else BUY
                        submit(LimitOrder(side_out, abs(pos), touch, ioc=True), t, bypass_risk=True)
                        flatten_order = next_id
                    continue

                # 4. The strategy sees this event, and only this event.
                k = kind[j]
                is_trade = k == _E or k == _C or k == _P
                snapshot = BookSnapshot(
                    t, chr(k), bid, bid_sz[j], ask, ask_sz[j],
                    trade_price[j] if is_trade else 0, shares[j] if is_trade else 0,
                    -side[j] if is_trade else 0, self.feed, start + j,
                )  # fmt: skip
                actions = strategy.on_book_update(snapshot)
                if actions:
                    for action in actions:
                        submit(action, t)

        # Anything still in flight arrives against the final book.
        sim.deliver_all(prev_row, prev_seq, prev_mid)
        if len(self.feed):
            last_t = int(self.feed.columns["timestamp"][-1])
            equity_samples.append((last_t, portfolio.equity(mid)))

        notes = []
        if account.position:
            notes.append(f"ended with an open position of {account.position} shares, "
                         "marked at the last mid")  # fmt: skip
        if sim.resting:
            notes.append(f"{len(sim.resting)} orders still resting at the end of the data")
        return build_result(config=cfg, symbol=self.feed.symbol, portfolio=portfolio,
                            simulator=sim, equity_samples=equity_samples, final_mid=mid,
                            orders=orders, rejected=rejected, notes=notes)  # fmt: skip
