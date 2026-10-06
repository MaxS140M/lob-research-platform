# Event-driven backtester

`lobpy.backtest` answers one question honestly: *would this strategy have made money on that
day, after costs?* It replays every book event for a symbol through the C++ engine and lets a
strategy react to each event, one at a time.

## Design

Four parts, each with one job:

| Part | Code | Job |
|---|---|---|
| **Data feed** | [`EventFeed`](../python/lobpy/backtest/feed.py), from `Replayer.events()` in C++ | Every book event for one symbol, in time order, with the book after it |
| **Strategy** | [`Strategy`](../python/lobpy/backtest/types.py) protocol | Sees one `BookSnapshot` at a time, returns orders and cancels |
| **Execution simulator** | [`ExecutionSimulator`](../python/lobpy/backtest/execution.py) | Latency, spread crossing, queue position, fees |
| **Portfolio** | [`Portfolio`](../python/lobpy/backtest/portfolio.py) | Position, cash, PnL, the risk limit |

```python
class Strategy(Protocol):
    def on_book_update(self, book: BookSnapshot) -> list[Order] | None: ...
    def on_fill(self, fill: Fill) -> None: ...
```

**Seeing only the past.** A strategy is never given the feed itself. The engine calls it with
the snapshot of the current event. Orders it sends only meet the book after the configured
latency, against the book as it stood when they arrived.

**Exact accounting.** Prices stay integers with 4 implied decimals from the ITCH file through
to PnL, so cash, fees and positions are exact.

## How fills are simulated

| Rule | What the simulator does |
|---|---|
| **Latency** | An order or cancel sent at time *t* reaches the exchange at *t* + latency. It is handled against the book after the last event at or before that time. |
| **Crossing the spread** | Market orders, and the marketable part of limit orders, walk the opposite side level by level: buys pay the asks, sells receive the bids. Market and IOC remainders are cancelled. |
| **Queue position** | A passive order joins the **back** of the queue: *ahead* = displayed size at its price on arrival. |
| **Moving up the queue** | Every event carries the affected order's queue priority from the C++ engine. Only executions, cancels, deletes and replaces of orders that were **already there** reduce *ahead*. Orders that arrive later sit behind. |
| **Passive fills** | The order fills, at its own price, when: an order **behind** it executes (FIFO means the aggressor reached it first); a hidden order trades at its price with nothing ahead; the market trades **through** its price; or the opposite side reaches its price. |
| **Fees** | Per share, set separately for taking and providing liquidity. |
| **Risk limit** | An order is rejected if the position could exceed `max_position` with every open order filled. |
| **Forced flat** | From 15:55 the backtester cancels everything and sends IOC orders **at the touch**, event after event, until flat. |

**Assumptions used for the results below:**

| Assumption | Value |
|---|---|
| Fees | $0.0030 per share for both taking and providing liquidity |
| Position limit | 1,000 shares |
| Forced flat | from 15:55 |
| Data | NASDAQ BX, 30 July 2019, 09:30–16:00, 5 book levels |

The fee is a flat, conservative assumption, not BX's actual schedule. BX is an "inverted"
venue that pays takers and charges makers, so per-share costs differ by venue and volume
tier.

**Simplifications, all against the strategy:**

- **No market impact:** the strategy's orders are virtual and never enter the replayed
  book.
- **Hidden orders:** assumed to have no priority over the strategy's displayed order, so
  only the trade-through and "nothing ahead" rules can fill against them.
- **Deep prices:** a price deeper than the visible depth has an unknown queue, so an order
  there only fills on a trade-through or a cross.
- **Crosses:** opening and closing crosses never fill strategy orders.

## Testing the backtester itself

[`test_backtest.py`](../python/tests/test_backtest.py) has 19 tests, every expected number
worked out by hand.

**Buy at the open, sell at the close,** on the hand-built
[`testdata/sample.itch`](../testdata/README.md):

| Fill | Price | Mid at fill | Spread | Signal | Fee |
|---|---:|---:|---:|---:|---:|
| Buy 100 at 11 s | $10.01 | $10.005 | −$0.50 | | $0.30 |
| Sell 100 at the forced flat | $9.99 | $10.00 | −$1.00 | | $0.30 |
| **Total** | | | **−$1.50** | **−$0.50** | **$0.60** |

The signal is the mid moving from $10.005 to $10.00, × 100 shares. Net PnL is −$2.00
gross − $0.60 fees = **−$2.60**. The backtester reproduces every number exactly.

The other tests use hand-written event sequences, one per rule:

- **Latency:** 10 ns vs 100 ns latency meet different books.
- **Spread crossing:** market orders walk the book; marketable limits rest their remainder;
  IOC cancels it.
- **Queue position, step by step:** partial cancels and executions ahead move the order up,
  but cancels behind it don't. An execution behind it fills it, as does a trade-through or
  the opposite side reaching its price.
- **Replace:** a replace ahead frees the old place in the queue.
- **Cancels:** a cancel only works after its own latency.
- **Fees and rebates:** per share, by liquidity type.
- **Risk:** the limit counts open orders.
- **Forced flat:** it never walks a thin book.
- **PnL identity:** net = signal + spread − fees holds on every run.

## Results on a real day

Run with
`python3 scripts/run_backtests.py data/20190730.BX_ITCH_50 SPY AAPL`. A full-day backtest
over ~200k events takes about 0.2 s.

### The strategies

- **Buy open, sell close:** buys 100 shares at the first event after 09:30. The forced flat
  sells them.
- **Imbalance taker:** when top-of-book imbalance |(bid − ask)/(bid + ask)| ≥ 0.8, it crosses
  the spread with an IOC at the touch for 100 shares, in the imbalance's direction. It holds
  for 10 s, exits with IOCs at the touch, and waits 1 s before trading again.
- **Imbalance maker:** the same signal, but it joins the queue at the best price on its side.
  If not filled in 2 s it cancels; once filled it holds for 10 s and exits at the touch.

### SPY (203,736 events)

| Strategy | Latency | Net PnL | Signal | Spread | Fees | Sharpe | Max DD | Fill rate | Turnover |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Buy open, sell close | 10 µs | $63.40 | $68.50 | −$4.50 | −$0.60 | 9.8 | $75.50 | 100% | 0.2× |
| Buy open, sell close | 100 µs | $63.40 | $68.50 | −$4.50 | −$0.60 | 9.8 | $75.50 | 100% | 0.2× |
| Buy open, sell close | 1 ms | $63.40 | $68.50 | −$4.50 | −$0.60 | 9.8 | $75.50 | 100% | 0.2× |
| Imbalance taker | 10 µs | −$1,825.13 | $251.56 | −$1,785.76 | −$290.93 | −268.2 | $1,840.16 | 91% | 97.0× |
| Imbalance taker | 100 µs | −$1,823.91 | $221.50 | −$1,758.87 | −$286.54 | −274.8 | $1,840.14 | 89% | 95.5× |
| Imbalance taker | 1 ms | −$1,764.93 | $202.58 | −$1,689.63 | −$277.88 | −268.2 | $1,780.83 | 84% | 92.6× |
| Imbalance maker | 10 µs | −$122.00 | $15.50 | −$110.50 | −$27.00 | −50.0 | $122.00 | 9% | 9.0× |
| Imbalance maker | 100 µs | −$126.00 | $13.50 | −$112.50 | −$27.00 | −52.0 | $126.00 | 9% | 9.0× |
| Imbalance maker | 1 ms | −$132.20 | $33.50 | −$137.50 | −$28.20 | −53.7 | $132.20 | 9% | 9.4× |

### AAPL (190,747 events)

| Strategy | Latency | Net PnL | Signal | Spread | Fees | Sharpe | Max DD | Fill rate | Turnover |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Buy open, sell close | 10 µs | $7.40 | $65.80 | −$57.80 | −$0.60 | 0.3 | $544.50 | 75% | 0.2× |
| Buy open, sell close | 100 µs | $7.40 | $65.80 | −$57.80 | −$0.60 | 0.3 | $544.50 | 75% | 0.2× |
| Buy open, sell close | 1 ms | −$41.70 | $41.25 | −$82.35 | −$0.60 | −1.1 | $559.90 | 70% | 0.2× |
| Imbalance taker | 10 µs | −$810.39 | $55.42 | −$837.14 | −$28.67 | −65.6 | $855.55 | 22% | 9.6× |
| Imbalance taker | 100 µs | −$815.72 | $73.49 | −$860.00 | −$29.21 | −66.1 | $858.53 | 22% | 9.7× |
| Imbalance taker | 1 ms | −$798.68 | $73.89 | −$841.40 | −$31.17 | −66.0 | $833.22 | 23% | 10.4× |
| Imbalance maker | 10 µs | −$504.56 | −$21.65 | −$442.64 | −$40.27 | −72.8 | $532.26 | 15% | 13.4× |
| Imbalance maker | 100 µs | −$510.21 | −$22.34 | −$447.32 | −$40.54 | −72.9 | $537.91 | 16% | 13.5× |
| Imbalance maker | 1 ms | −$514.19 | −$11.84 | −$460.01 | −$42.34 | −71.1 | $541.89 | 17% | 14.1× |

### What the numbers say

- **The signal is real, but the spread eats it.** The imbalance taker's *signal* PnL is
  positive for both symbols: prices did move the way the imbalance pointed. But crossing the
  spread on every entry and exit costs 7 to 15 times what the signal earns. In
  this form the strategy loses money, and the breakdown shows exactly why.
- **Providing liquidity has its own cost: adverse selection.** The maker earns the spread on
  entry, but fills only 9–17% of the time. Those fills come disproportionately when the
  market is about to move through its price, and on AAPL its signal PnL turns negative. A
  passive fill happens because someone was willing to trade against you, which is
  information.
- **Latency barely matters at a 10-second horizon.** The 10 µs to 1 ms results differ by a
  few percent, within the noise of which IOCs happened to find liquidity. Latency starts to
  matter when the holding period approaches the time it takes the book to change.
- **Buy-and-hold is mostly market direction.** SPY rose about $0.68 between the open and
  15:55. On AAPL, BX's thin book at the open (a $1.09 spread at 09:30:00) and at the close
  makes the single round trip cost $58–82 in spread. The flatten needed several IOCs, hence
  the fill rate below 100%.

**How to read the metrics:**

| Metric | Meaning here |
|---|---|
| **Signal / spread split** | Spread is each fill's price versus the mid at that moment. For passive fills that happen because the market moved through the order's price, the mid has already moved, so adverse selection at the moment of the fill shows up under spread. |
| **Sharpe** | Mean / std of 1-minute PnL changes × √(252 × 390). On one day, a strategy that loses steadily gets an extreme value. It is reported because the guide asks for it, but only means anything over many days. |
| **Fill rate** | Filled shares / submitted shares. Repeated exit and flatten IOCs count as submissions. |
| **Turnover** | Shares traded / the 1,000-share position limit. |

## Limitations

- **One venue.** This file is NASDAQ BX only. BX's displayed book is thin: AAPL had 45 shares
  at the best bid at 15:55, with the next bid $4.91 lower. Real execution routes across
  exchanges to the national best price. Results here are what a BX-only trader would have
  got, which is pessimistic. The full NASDAQ TotalView files (5–12 GB) have much deeper
  books.
- **One day.** Nothing here is a statistically meaningful estimate of a strategy's
  performance. The ML phase trains and tests across several days.
- **No market impact.** The strategy's orders don't change the replayed book or other
  traders' behaviour. At 100 shares that is a reasonable approximation; at size it isn't.
