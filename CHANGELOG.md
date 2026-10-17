# Changelog

## [Unreleased]

## [0.4.0] - 2026-10-17

Python bindings.

### Added

- `lobpy.Replayer`, pybind11 bindings to a new C++ `lob::Replayer` that replays an ITCH file
  for chosen symbols. ([#9])
  - `snapshots(interval_ns, depth, start_ns, end_ns)` samples the top N levels on a fixed
    time grid.
  - `trades()` returns every execution with its aggressor side, match number and kind.
  - Results are dicts of **zero-copy NumPy arrays**: C++ fills preallocated buffers, and a
    capsule hands their ownership to NumPy.
  - The GIL is released during the replay.
  - Requests whose output would exceed 4 GiB are rejected with a clear error.
- `lobpy.snapshots_frame` and `lobpy.trades_frame`, which convert results to pandas
  DataFrames with dollar prices, mid and spread. ([#9])
- `testdata/sample.itch`, a hand-built ITCH day with documented expected books and trades,
  plus its generator `scripts/make_sample_itch.py`. The C++ and Python tests assert the same
  values against it. ([#9])
- Notebook `notebooks/01_replay_snapshots.ipynb`. It replays the BX day from Python, charts
  SPY's mid and spread, and previews the order book imbalance signal. ([#9])

### Changed

- The Python package now builds with scikit-build-core. `pip install .` compiles the C++
  engine and the extension through the existing CMake file, with tests, tools and benchmarks
  turned off. ([#9])

## [0.3.0] - 2026-10-17

Order book, matching engine and benchmarks.

### Added

- `lob::OrderBook`, a single-instrument limit order book with price-time priority. It
  supports add, reduce, remove, cancel-replace (which loses time priority), top of book,
  depth, and front-of-queue access for matching. ([#5])
- `lob::BookBuilder`, an ITCH `parse()` handler that rebuilds a book per stock locate and
  counts anomalies instead of failing. ([#5])
- `book_replay` tool. It replays a full day and reports anomalies, locked or crossed books
  by session, top of book at the 16:00 close, and a top-of-book digest over every update.
  On the 30 July 2019 BX day it finds 0 anomalies, no locked or crossed books, and closing
  quotes that match that day's real prices. ([#5], [#7])
- `lob::MatchingEngine`, with price-time priority and limit, market, IOC and cancel orders.
  Each trade emits a `Fill` event (price, quantity, aggressor side, both order ids) through
  a compile-time callback. Self-trade prevention can cancel either the incoming or the
  resting order. ([#6])
- `lob::OrderIndex`, an open-addressing hash map with linear probing, Fibonacci hashing and
  backward-shift deletion. ([#8])
- `lob_bench`, a Google Benchmark suite:
  - Per-operation p50 / p99 / p99.9 latency for add, cancel and match, read with `rdtsc`
    into a one-cycle histogram.
  - Latency of every book update in a real ITCH day, by message type.
  - Full-day replay throughput.
  - Same-run comparisons against the original implementation. ([#7], [#8])
- `-DLOB_NATIVE=ON` CMake option for `-march=native` builds. ([#7])
- `scripts/bench_summary.py`, which turns benchmark JSON into Markdown tables. ([#7])
- Tests:
  - Edge cases for the matching engine.
  - A differential test of the matching engine against a brute-force model over 100,000
    random orders.
  - A differential test of `OrderBook` against the frozen original implementation.
  - Unit and churn tests for `OrderIndex`. ([#5], [#6], [#8])

### Changed

- The order book was optimised in three benchmarked steps, each profiled with `perf`:
  - **Price levels:** flat sorted vectors instead of `std::map`.
  - **Orders:** a pooled order store with intrusive per-level queues, instead of a
    `std::list` per level.
  - **Order index:** open addressing instead of `std::unordered_map`.

  Together, in the same run: full-day replay is 2.4× faster (12.5 to 30.3 M msg/s), and
  real-data add p99 latency fell from 210 to 120 ns. See
  [docs/benchmarks.md](docs/benchmarks.md). ([#8])

## [0.2.0] - 2026-10-17

ITCH 5.0 market data parser.

### Added

- ITCH 5.0 message definitions and decoders (`lob/itch.hpp`) for System Event, Stock
  Directory, Add Order (with and without MPID), Order Executed (with and without price), Order
  Cancel, Order Delete, Order Replace, Trade, Cross Trade and Broken Trade. Byte offsets are
  checked against the official specification. Prices are kept as integers with 4 implied
  decimals. ([#2])
- Stream parser `lob::itch::parse()` (`lob/itch_parser.hpp`). The handler is a template
  parameter, so callbacks are resolved at compile time: no virtual calls and no allocation per
  message. Message lengths are checked for all 23 ITCH 5.0 types, and parsing stops with an
  error code on truncated or malformed input. ([#2])
- `lob::MappedFile`, a read-only, memory-mapped view of a file (`mmap` with
  `MADV_SEQUENTIAL`). ([#2])
- `itch_stats` command-line tool. It reports message counts per type, checks that
  timestamps never go backwards and that the day runs from start to end of messages, and
  times the parser with and without full field decoding. ([#2])
- Parser benchmark on a full NASDAQ BX day (28.7M messages, 837 MB): 0.18 s with full field
  decoding, 6.3 ns per message. See [docs/benchmarks.md](docs/benchmarks.md). ([#2])
- 30 GoogleTest unit tests covering every decoder, the parser's handling of message
  boundaries and errors, and `MappedFile`. ([#2])
- CI check that C++ sources match `clang-format`. ([#2])
- This changelog. ([#3])

### Changed

- Compiler warnings raised to `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion` for all project targets. ([#2])
- Minimum CMake version raised to 3.25, so GoogleTest's headers are treated as system headers
  and excluded from the stricter warnings. ([#2])

### Removed

- Placeholder `lob::ItchParser` class, replaced by `lob::itch::parse()`. ([#2])

## [0.1.0] - 2026-10-16

Project skeleton, build and CI.

### Added

- `cpp/` and `python/` source trees, laid out for the engine and the research layer. ([#1])
- CMake build (C++20, Ninja) with GoogleTest. ([#1])
- `lobpy` Python package with pytest. ([#1])
- GitHub Actions CI running the C++ and Python test suites. ([#1])

[Unreleased]: https://github.com/MaxS140M/lob-research-platform/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/MaxS140M/lob-research-platform/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MaxS140M/lob-research-platform/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MaxS140M/lob-research-platform/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MaxS140M/lob-research-platform/releases/tag/v0.1.0
[#1]: https://github.com/MaxS140M/lob-research-platform/pull/1
[#2]: https://github.com/MaxS140M/lob-research-platform/pull/2
[#3]: https://github.com/MaxS140M/lob-research-platform/pull/3
[#5]: https://github.com/MaxS140M/lob-research-platform/pull/5
[#6]: https://github.com/MaxS140M/lob-research-platform/pull/6
[#7]: https://github.com/MaxS140M/lob-research-platform/pull/7
[#8]: https://github.com/MaxS140M/lob-research-platform/pull/8
[#9]: https://github.com/MaxS140M/lob-research-platform/pull/9
