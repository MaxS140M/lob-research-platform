# Changelog

## [Unreleased]

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

[Unreleased]: https://github.com/MaxS140M/lob-research-platform/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/MaxS140M/lob-research-platform/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MaxS140M/lob-research-platform/releases/tag/v0.1.0
[#1]: https://github.com/MaxS140M/lob-research-platform/pull/1
[#2]: https://github.com/MaxS140M/lob-research-platform/pull/2
[#3]: https://github.com/MaxS140M/lob-research-platform/pull/3
