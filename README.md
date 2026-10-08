# lob-research-platform

[![CI](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml/badge.svg)](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

C++ limit order book engine on NASDAQ ITCH data, with a Python backtester and ML order-flow signal.

A small version of the research stack a trading firm runs internally. A fast C++20 engine
replays a full day of NASDAQ TotalView-ITCH 5.0 data and rebuilds the order book. A Python
layer drives that engine to backtest an ML short-horizon price signal.

> **Status:** the ITCH 5.0 parser, order book, matching engine, Python bindings, event-driven
> backtester and ML signal study are complete. See [CHANGELOG.md](CHANGELOG.md) for releases.

## Highlights

- **ML signal study:** order book features predict the next mid-price move out of sample.
  - **Data:** five NASDAQ BX days, 2.3M snapshots.
  - **Validation:** walk-forward with an embargo, plus one holdout day used once, and a test
    that proves no feature uses future data.
  - **Results:** LightGBM beats every baseline on log-loss at 1 s, 5 s and 30 s. Traded
    through the backtester, though, the edge lives where the spread is widest. A diagnosis
    and a cost-aware variant show why the signal, though real, doesn't survive costs on
    this venue.
  - Write-up: [docs/research.md](docs/research.md).

- **Order book replay:** rebuilds every book (8,850 instruments) from a full NASDAQ BX day at
  **26–30 M messages/s**, **2.4× faster** than the `std::map` / `std::list` /
  `std::unordered_map` baseline measured in the same run. On real data, a book update takes
  about **50 ns at p50 and 120 ns at p99**.
- **Correct on real data:** 0 anomalies and no locked or crossed books across 28.7M
  messages. Top of book at the close matches that day's real prices. Every optimisation
  reproduces the baseline's top-of-book digest exactly.
- **Optimised one change at a time**, each step profiled with `perf` and benchmarked:
  - **v2:** flat sorted price-level vectors.
  - **v3:** an order pool with intrusive queues.
  - **v4:** an open-addressing order index.
- **Matching engine:** price-time priority with limit, market, IOC and cancel orders, fill
  events and self-trade prevention. It's checked against a brute-force model over 100,000
  random orders.
- **Event-driven backtester:** a strategy sees one book event at a time, and its orders face
  realistic fills:
  - **latency** (10 µs to 1 ms)
  - **spread crossing** level by level
  - **queue position** tracked order by order, using queue priorities from the C++ engine
  - **fees**, **risk limits** and a **forced flat** before the close

  Results come with Sharpe, drawdown, fill rate, turnover, and a **PnL breakdown into
  signal, spread and fees**. On real data it shows why an order book imbalance signal that
  predicts correctly still loses money once it pays the spread. See
  [docs/backtest.md](docs/backtest.md).
- **Python bindings (pybind11):** `lobpy.Replayer` replays the day in C++ and returns order
  book snapshots and trades as **zero-copy NumPy arrays**, with the GIL released during the
  replay. One-second, five-level snapshots for four stocks over the trading day take
  **0.25 s**. See the [notebook](notebooks/01_replay_snapshots.ipynb).
- **ITCH 5.0 parser:** parses the full day (837 MB) in **0.18 s with full field decoding**,
  6.3 ns per message. The file is memory-mapped and read without copying.
- **Benchmark method:** latency measured per operation with `rdtsc` (p50 / p99 / p99.9),
  pinned to one core, median of 5 repetitions. Timer overhead, timer resolution and noise
  are all reported. See [docs/benchmarks.md](docs/benchmarks.md).

## Repository layout

```
cpp/
  include/lob/   public headers
  src/           implementation
  tests/         GoogleTest unit tests
  bench/         Google Benchmark speed tests
  tools/         command-line tools (itch_stats, book_replay)
python/
  lobpy/         Python package: bindings, DataFrame helpers (backtester and models to come)
  tests/         pytest tests
notebooks/       research notebooks
testdata/        tiny hand-built ITCH file shared by the C++ and Python tests
scripts/         sample-file generator, benchmark summary
data/            raw ITCH files (not committed)
docs/            benchmarks and write-ups
```

## Building

Requires a C++20 compiler (GCC 12+ or Clang 15+), CMake 3.25+ and Ninja. Linux or WSL2 is
recommended.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On WSL2, keep the build directory on the Linux filesystem (for example
`cmake -S . -B ~/build/lob ...`) if the source tree is under `/mnt/c`. Building there is faster
and avoids Windows file locks during dependency extraction.

## Usage

Parse a full ITCH 5.0 file and print per-type message counts, timestamp and session sanity
checks, and parse throughput:

```bash
./build/itch_stats data/20190730.BX_ITCH_50
```

From C++, implement only the callbacks you need. Dispatch is resolved at compile time:

```cpp
#include "lob/itch_parser.hpp"
#include "lob/mapped_file.hpp"

struct MyHandler {
    void on_add_order(const lob::itch::AddOrder& m) { /* ... */ }
    void on_order_delete(const lob::itch::OrderDelete& m) { /* ... */ }
};

lob::MappedFile file("data/20190730.BX_ITCH_50");
MyHandler handler;
auto result = lob::itch::parse(file.bytes(), handler);
```

Rebuild every order book for the day and check it (anomalies, locked or crossed books,
top-of-book digest, top of book at the close):

```bash
./build/book_replay data/20190730.BX_ITCH_50
```

Run your own orders through the matching engine:

```cpp
#include "lob/matching_engine.hpp"

lob::MatchingEngine engine;
engine.submit({1, lob::Side::kSell, lob::OrderType::kLimit, 1'000'000, 100});  // $100.00
auto result = engine.submit({2, lob::Side::kBuy, lob::OrderType::kImmediateOrCancel,
                             1'000'100, 150},
                            [](const lob::Fill& f) { /* price, quantity, both ids */ });
// result.filled == 100, result.cancelled == 50
```

Benchmarks (see [docs/benchmarks.md](docs/benchmarks.md) for the full method):

```bash
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON
cmake --build build-native
LOB_ITCH_FILE=data/20190730.BX_ITCH_50 taskset -c 2 ./build-native/lob_bench
```

## Python package

Requires Python 3.11+ and the C++ toolchain above. `pip` builds the C++ engine and its
pybind11 extension through the same CMake file, via scikit-build-core.

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install ".[dev]"          # or ".[dev,notebook]" to run the notebooks
pytest
```

The C++ is compiled at install time, so rerun `pip install .` after changing it.

```python
import lobpy

r = lobpy.Replayer("data/20190730.BX_ITCH_50", ["AAPL", "MSFT"])
snaps = r.snapshots(interval_ns=1_000_000_000, depth=5)   # dict of NumPy arrays, zero-copy
trades = r.trades()                                       # every execution, with aggressor side

df = lobpy.snapshots_frame(snaps, r.symbols)              # pandas: bid_px_1..5, mid, spread, ...
```

Prices stay integers with 4 implied decimals (`int64`) in the raw arrays. The DataFrame helpers
convert them to dollars.

### Backtesting a strategy

```python
import lobpy
from lobpy.backtest import BUY, BacktestConfig, Backtester, EventFeed, LimitOrder

class JoinTheBid:
    """Buy 100 at the best bid once; the backtester flattens before the close."""
    def __init__(self):
        self.sent = False
    def on_book_update(self, book):              # sees one event at a time, never the future
        if not self.sent and book.bid_price:
            self.sent = True
            return [LimitOrder(BUY, 100, book.bid_price)]   # joins the back of the queue
    def on_fill(self, fill):
        print(fill)

feed = EventFeed.from_replayer(lobpy.Replayer("data/20190730.BX_ITCH_50", ["SPY"]), "SPY")
result = Backtester(feed, BacktestConfig(latency_ns=100_000)).run(JoinTheBid())
print(result.summary())     # net PnL split into signal / spread / fees, Sharpe, drawdown, ...
```

`python3 scripts/run_backtests.py data/20190730.BX_ITCH_50 SPY AAPL` reproduces the tables in
[docs/backtest.md](docs/backtest.md).

## Data

See [data/README.md](data/README.md) for where to download sample ITCH 5.0 files.

## License

[MIT](LICENSE)
