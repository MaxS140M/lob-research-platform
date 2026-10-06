"""Run the example strategies on a real ITCH day across latencies, and print Markdown tables.

usage: python3 scripts/run_backtests.py <itch-file> [SYMBOL ...]

Used to produce docs/backtest.md.
"""

from __future__ import annotations

import sys
import time

import lobpy
from lobpy.backtest import HOUR, MINUTE, SECOND, BacktestConfig, Backtester, EventFeed
from lobpy.backtest.strategies import BuyOpenSellClose, ImbalanceMaker, ImbalanceTaker

OPEN = 9 * HOUR + 30 * MINUTE
CLOSE = 16 * HOUR
LATENCIES = {"10 us": 10_000, "100 us": 100_000, "1 ms": 1_000_000}

STRATEGIES = {
    "Buy open, sell close": lambda: BuyOpenSellClose(quantity=100, open_ns=OPEN),
    "Imbalance taker": lambda: ImbalanceTaker(threshold=0.8, quantity=100, hold_ns=10 * SECOND),
    "Imbalance maker": lambda: ImbalanceMaker(threshold=0.8, quantity=100, hold_ns=10 * SECOND,
                                              patience_ns=2 * SECOND),
}  # fmt: skip


def main() -> None:
    path = sys.argv[1]
    symbols = sys.argv[2:] or ["SPY"]
    replayer = lobpy.Replayer(path, symbols)

    header = ("| Strategy | Latency | Net PnL | Signal | Spread | Fees | Sharpe | Max DD "
              "| Fill rate | Turnover | Round trips |")  # fmt: skip
    for symbol in symbols:
        t0 = time.perf_counter()
        feed = EventFeed.from_replayer(replayer, symbol, depth=5, start_ns=OPEN, end_ns=CLOSE)
        print(f"\n### {symbol}: {len(feed):,} events (fetched in {time.perf_counter() - t0:.2f} s)\n")
        print(header)
        print("|---|---|" + "---:|" * 9)
        for name, make in STRATEGIES.items():
            for label, latency in LATENCIES.items():
                t0 = time.perf_counter()
                result = Backtester(feed, BacktestConfig(latency_ns=latency)).run(make())
                elapsed = time.perf_counter() - t0
                m = result.metrics
                trips = m["traded_shares"] / 200
                print(
                    f"| {name} | {label} | ${m['net_pnl']:,.2f} | ${m['signal_pnl']:,.2f} "
                    f"| ${m['spread_pnl']:,.2f} | ${-m['fees']:,.2f} | {m['sharpe']:.1f} "
                    f"| ${m['max_drawdown']:,.2f} | {m['fill_rate']:.0%} | {m['turnover']:.1f}x "
                    f"| {trips:,.0f} |",
                    flush=True,
                )
                print(f"<!-- {symbol} {name} {label}: {elapsed:.2f} s -->", file=sys.stderr)


if __name__ == "__main__":
    main()
