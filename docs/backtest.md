# Backtester

A backtester answers: *would this strategy have made money on that day, after costs?* This
one replays every order book event of a day, one at a time, and simulates how your orders
would really have been filled.

## How it works

Four parts, each with one job:

| Part | Job |
|---|---|
| **Data feed** | Every book event for one stock, in time order, from the C++ engine |
| **Strategy** | Sees one book update at a time and decides whether to send orders. It never sees the future. |
| **Execution simulator** | Decides if, when and at what price each order fills |
| **Portfolio** | Tracks position, cash, profit and loss, and enforces a position limit |

A strategy is any class with two methods:

```python
class MyStrategy:
    def on_book_update(self, book):   # called for every event; return a list of orders or None
        ...
    def on_fill(self, fill):          # called when one of your orders trades
        ...
```

See the [README](../README.md#quick-start) or `scripts/run_backtests.py` for complete examples.

## How fills are simulated

| Effect | What happens |
|---|---|
| **Latency** | An order sent at time *t* reaches the exchange at *t* + latency (default 100 µs). It then meets the book as it was at that moment. |
| **Crossing the spread** | Buying at market pays the ask; selling receives the bid. Big orders walk down through several price levels. |
| **Queue position** | A resting order joins the back of the queue at its price. It only moves forward when orders that were already ahead of it trade or cancel. |
| **Passive fills** | A resting order fills when someone trades against its price level after the orders ahead are gone, or when the price moves through it. |
| **Fees** | $0.003 per share, every fill. This is an assumption, not BX's real fee schedule. |
| **Risk limit** | Orders that could take the position beyond 1,000 shares are rejected. |
| **Close-out** | From 15:55 the backtester cancels everything and closes the position, a little at a time, at the best price. |

**Simplifications (all on the cautious side):**
- **No market impact:** your orders don't change the replayed market.
- **Hidden orders:** they never let your order jump the queue.
- **Auctions:** opening and closing auctions never fill your orders.

## The results it reports

| Metric | Meaning |
|---|---|
| **Net PnL** | Profit after everything |
| **Signal PnL** | Profit from prices moving your way after you traded |
| **Spread PnL** | What you paid to cross the spread (or earned by waiting for others to trade with you) |
| **Fees** | Exchange fees paid |
| **Sharpe** | Return per unit of risk. On one day it's not very meaningful. |
| **Max drawdown** | Worst fall from a high point |
| **Fill rate** | Share of submitted shares that actually traded |
| **Turnover** | Shares traded, relative to the position limit |

**Net = signal + spread − fees**, always. This split is what explains *why* a strategy made
or lost money.

## Is the backtester itself correct?

Every rule has a test with numbers worked out by hand: 19 tests in
[test_backtest.py](../python/tests/test_backtest.py).

The main end-to-end check is **"buy 100 shares at the open, sell at the close"** on the tiny
sample file:

| | Price | Mid at the time | Cost of the spread | Fee |
|---|---:|---:|---:|---:|
| Buy 100 | $10.01 | $10.005 | −$0.50 | $0.30 |
| Sell 100 | $9.99 | $10.00 | −$1.00 | $0.30 |

- **Signal:** the mid fell from $10.005 to $10.00, which on 100 shares is −$0.50.
- **Net:** −$0.50 signal − $1.50 spread − $0.60 fees = **−$2.60**.

The backtester reproduces every number.

## Results on a real day (30 July 2019)

Three simple strategies on SPY. Each runs in about 0.2 seconds for a whole day of about
200,000 events.

| Strategy | Net PnL | Signal | Spread | Fees | Fill rate |
|---|---:|---:|---:|---:|---:|
| Buy at the open, sell at the close | $63 | $69 | −$5 | −$1 | 100% |
| Imbalance taker: cross the spread when buyers or sellers heavily outnumber the other side | −$1,824 | $222 | −$1,759 | −$287 | 89% |
| Imbalance maker: same signal, but wait in the queue | −$126 | $14 | −$113 | −$27 | 9% |

All at 100 µs latency. The results at 10 µs and 1 ms are almost identical (see
[run_backtests.py](../scripts/run_backtests.py)).

**What this shows:**
- **The signal is real, but crossing the spread costs more than it earns.** The imbalance
  taker predicted the direction correctly often enough to make $222. It paid $1,759 to get
  in and out.
- **Waiting in the queue has a hidden cost.** The maker only gets filled 9% of the time.
  Those fills tend to happen just before the price moves against it, which is called
  *adverse selection*.
- **Latency barely matters at this horizon.** The trades last 10 seconds, so 10 µs vs 1 ms
  makes no real difference.

## Limitations

- **One exchange:** this data covers NASDAQ BX only, which has thin books. For example,
  AAPL had just 45 shares at the best bid at 15:55. Real traders route orders across all
  exchanges.
- **One day:** a single day can't tell you whether a strategy works in general.
- **No market impact:** fine for 100 shares, wrong for large orders.
