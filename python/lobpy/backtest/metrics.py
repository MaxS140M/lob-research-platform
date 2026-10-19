"""Results of a backtest, judged the way a trading desk would."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from .types import MINUTE, BacktestConfig

DOLLAR = 10_000  # price units per dollar
MINUTES_PER_YEAR = 252 * 390  # trading days x minutes per regular session


@dataclass
class BacktestResult:
    config: BacktestConfig
    metrics: dict[str, float]
    fills: pd.DataFrame
    equity: pd.Series  # net mark-to-market PnL in dollars, sampled every second
    symbol: str = ""
    notes: list[str] = field(default_factory=list)

    def summary(self) -> str:
        m = self.metrics
        lines = [
            f"{self.symbol or 'backtest'}: latency {self.config.latency_ns / 1000:g} us, "
            f"fees ${self.config.taker_fee:.4f} taker / ${self.config.maker_fee:.4f} maker per share",
            f"  net PnL        ${m['net_pnl']:>12,.2f}",
            f"    signal       ${m['signal_pnl']:>12,.2f}   (price moves after each fill)",
            f"    spread       ${m['spread_pnl']:>12,.2f}   (paid or earned vs mid at fill)",
            f"    fees         ${-m['fees']:>12,.2f}",
            f"  Sharpe         {m['sharpe']:>13.2f}   (annualised from 1-minute PnL, one day)",
            f"  max drawdown   ${m['max_drawdown']:>12,.2f}",
            f"  fill rate      {m['fill_rate']:>13.1%}   ({m['filled_shares']:,.0f} of "
            f"{m['submitted_shares']:,.0f} shares)",
            f"  turnover       {m['turnover']:>13.1f}x  (shares traded / position limit)",
            f"  orders         {m['orders']:>13,.0f}   ({m['rejected_orders']:,.0f} rejected by risk)",
            f"  max |position| {m['max_abs_position']:>13,.0f}   (limit {self.config.max_position:,})",
        ]
        lines += [f"  note: {n}" for n in self.notes]
        return "\n".join(lines)


def build_result(*, config, symbol, portfolio, simulator, equity_samples, final_mid, orders,
                 rejected, notes) -> BacktestResult:  # fmt: skip
    a = portfolio.account
    net_units = portfolio.equity(final_mid)
    fees = a.fees / DOLLAR
    net = net_units / DOLLAR
    gross = net + fees
    spread = portfolio.spread_pnl / DOLLAR

    times, values = zip(*equity_samples) if equity_samples else ((), ())
    equity = pd.Series(np.asarray(values, dtype=float) / DOLLAR,
                       index=pd.to_timedelta(np.asarray(times, dtype=np.int64), unit="ns"),
                       name="net_pnl")  # fmt: skip

    metrics = {
        "net_pnl": net,
        "gross_pnl": gross,
        "signal_pnl": gross - spread,
        "spread_pnl": spread,
        "fees": fees,
        "sharpe": _sharpe(equity),
        "max_drawdown": _max_drawdown(equity),
        "fill_rate": (simulator.filled_shares / simulator.submitted_shares
                      if simulator.submitted_shares else math.nan),  # fmt: skip
        "submitted_shares": simulator.submitted_shares,
        "filled_shares": simulator.filled_shares,
        "turnover": portfolio.traded_shares / config.max_position,
        "traded_shares": portfolio.traded_shares,
        "orders": orders,
        "rejected_orders": rejected,
        "max_abs_position": portfolio.max_abs_position,
        "final_position": a.position,
    }

    fills = pd.DataFrame(
        [(f.timestamp, f.order_id, f.side, f.price / DOLLAR, f.quantity, f.fee / DOLLAR,
          f.liquidity, f.mid / DOLLAR) for f in portfolio.fills],
        columns=["timestamp", "order_id", "side", "price", "quantity", "fee", "liquidity", "mid"],
    )  # fmt: skip
    fills.insert(0, "time", pd.to_timedelta(fills.pop("timestamp"), unit="ns"))
    return BacktestResult(config, metrics, fills, equity, symbol, notes)


def _sharpe(equity: pd.Series) -> float:
    """Mean / std of 1-minute PnL changes, annualised. With one day of data this is noisy."""
    if len(equity) < 3:
        return math.nan
    minute = (equity.index.asi8 // MINUTE)
    per_minute = equity.groupby(minute).last()
    changes = per_minute.diff().dropna()
    std = changes.std()
    if len(changes) < 2 or not std > 0:
        return math.nan
    return float(changes.mean() / std * math.sqrt(MINUTES_PER_YEAR))


def _max_drawdown(equity: pd.Series) -> float:
    """Largest fall from a running peak of net PnL, in dollars (a positive number)."""
    if equity.empty:
        return 0.0
    values = equity.to_numpy()
    peaks = np.maximum.accumulate(np.maximum(values, 0.0))  # PnL starts at 0
    return float(np.max(peaks - values))
