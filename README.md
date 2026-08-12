# lob-research-platform

[![CI](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml/badge.svg)](https://github.com/MaxS140M/lob-research-platform/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

C++ limit order book engine on NASDAQ ITCH data, with a Python backtester and ML order-flow signal.

A small version of the research stack a trading firm runs internally. A fast C++20 engine
replays a full day of NASDAQ TotalView-ITCH 5.0 data and rebuilds the order book. A Python
layer drives that engine to backtest an ML short-horizon price signal.

> **Status:** early development. The project skeleton, build and CI are in place; components
> are being added phase by phase (see [Roadmap](#roadmap)).

## Roadmap

| Phase | Component                                             | Status      |
| ----- | ----------------------------------------------------- | ----------- |
| 0     | Project skeleton, CMake build, test harness, CI       | Done        |
| 1     | ITCH 5.0 binary parser                                | Planned     |
| 2     | Limit order book and matching engine, with benchmarks | Planned     |
| 3     | pybind11 Python bindings                              | Planned     |
| 4     | Event-driven backtester with realistic costs          | Planned     |
| 5     | Order book features and ML price-move model           | Planned     |
| 6     | Results write-up                                      | Planned     |

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
