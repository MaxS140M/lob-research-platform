# lob-research-platform

[![CI](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml/badge.svg)](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

C++ limit order book engine on NASDAQ ITCH data, with a Python backtester and ML order-flow signal.

A small version of the research stack a trading firm runs internally. A fast C++20 engine
replays a full day of NASDAQ TotalView-ITCH 5.0 data and rebuilds the order book. A Python
layer drives that engine to backtest an ML short-horizon price signal.

> **Status:** in development. The ITCH 5.0 parser is complete; the order book, Python bindings,
> backtester and ML signal are in progress. See [CHANGELOG.md](CHANGELOG.md) for releases.

## Highlights

- **ITCH 5.0 parser:** parses a full NASDAQ BX day (28.7M messages, 837 MB) in **0.18 s
  with full field decoding**, which is 6.3 ns and about 160M messages per second. The file
  is memory-mapped and read without copying. Callbacks are resolved at compile time, so
  there are no virtual calls and nothing is allocated per message. See
  [docs/benchmarks.md](docs/benchmarks.md).

## Repository layout

```
cpp/
  include/lob/   public headers
  src/           implementation
  tests/         GoogleTest unit tests
  bench/         Google Benchmark speed tests
python/
  lobpy/         Python package: backtester, features, models
  tests/         pytest tests
notebooks/       research notebooks
data/            raw ITCH files (not committed)
docs/            architecture, write-up, benchmark charts
```

## Building

Requires a C++20 compiler (GCC 12+ or Clang 15+), CMake 3.20+ and Ninja. Linux or WSL2 is
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

## Python package

Requires Python 3.11+.

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
pytest
```

## Data

See [data/README.md](data/README.md) for where to download sample ITCH 5.0 files.

## License

[MIT](LICENSE)
