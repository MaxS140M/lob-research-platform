"""Event-driven backtester on top of the C++ engine.

Four parts, each with one job:

- ``EventFeed``: every book event for one symbol, in time order, from ``Replayer.events``.
- ``Strategy``: sees one ``BookSnapshot`` at a time and returns orders.
- ``ExecutionSimulator``: latency, spread crossing, queue position, fees.
- ``Portfolio``: position, cash, PnL and the risk limit.

    >>> from lobpy.backtest import Backtester, BacktestConfig, EventFeed
    >>> from lobpy.backtest.strategies import ImbalanceTaker
    >>> feed = EventFeed.from_replayer(lobpy.Replayer(path, ["SPY"]), "SPY")
    >>> result = Backtester(feed, BacktestConfig(latency_ns=100_000)).run(ImbalanceTaker())
    >>> print(result.summary())
"""

from .engine import Backtester
from .execution import ExecutionSimulator
from .feed import EventFeed
from .metrics import BacktestResult
from .portfolio import Portfolio
from .types import (
    BUY,
    HOUR,
    MINUTE,
    SECOND,
    SELL,
    Account,
    BacktestConfig,
    BookSnapshot,
    Cancel,
    Fill,
    LimitOrder,
    MarketOrder,
    Strategy,
)

__all__ = [
    "BUY",
    "SELL",
    "SECOND",
    "MINUTE",
    "HOUR",
    "Account",
    "BacktestConfig",
    "BacktestResult",
    "Backtester",
    "BookSnapshot",
    "Cancel",
    "EventFeed",
    "ExecutionSimulator",
    "Fill",
    "LimitOrder",
    "MarketOrder",
    "Portfolio",
    "Strategy",
]
