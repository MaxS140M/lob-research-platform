# Benchmarks

## ITCH 5.0 parser

Full-day parse of a real NASDAQ BX TotalView-ITCH 5.0 file using `itch_stats`.

**Dataset:** `20190730.BX_ITCH_50`, NASDAQ BX, 30 July 2019. 837.5 MB uncompressed,
28,734,686 messages.

**Machine:** AMD Ryzen 5 5600X (6C/12T), WSL2 Ubuntu 24.04, GCC 13.3, `-O3`
(CMake `Release`). File on the WSL ext4 filesystem.

| Run                                    | Wall time | Throughput      | Per message |
| -------------------------------------- | --------: | --------------: | ----------: |
| Scan + count, cold page cache          |   1.659 s | 17.3 M msg/s    |     57.7 ns |
| Scan + count, warm page cache          |   0.128 s | 225 M msg/s     |      4.4 ns |
| Full field decode, warm page cache     |   0.180 s | 160 M msg/s     |      6.3 ns |

Warm figures are the median of 5 runs. The run-to-run spread was under 1%.

### What each run measures

- **Scan + count:** walks the length-prefixed framing, checks every message length
  against the spec, counts messages by type and checks that timestamps never go backwards.
  The cold run starts with the page cache dropped (`echo 3 > /proc/sys/vm/drop_caches`), so it
  includes reading the file from disk through `mmap` page faults. At about 505 MB/s it is
  limited by disk reads, not by the parser.
- **Full field decode:** also decodes every order-book and trade message (A, F, E, C, X, D,
  U, P, Q) into its struct. The decoded fields are folded into a checksum so the compiler cannot
  optimise the decoding away.

### Sanity checks on the full day

- **Message mix:** Add Order (`A`) is the most common message (36.8%), followed by
  Order Delete (`D`, 35.4%).
- **Timestamps:** never go backwards across all 28.7M messages.
- **Day boundaries:** the first system event is `O` (start of messages, 03:06:34) and the
  last is `C` (end of messages, 19:05:00). Market hours run from `Q` at 09:30:00 to `M` at
  16:00:00.

| Type | Message                     |      Count |  Share |
| ---- | --------------------------- | ---------: | -----: |
| A    | Add Order                   | 10,580,123 | 36.82% |
| D    | Order Delete                | 10,164,658 | 35.37% |
| N    | Retail Price Improvement    |  4,636,704 | 16.14% |
| U    | Order Replace               |  2,046,443 |  7.12% |
| E    | Order Executed              |    679,389 |  2.36% |
| X    | Order Cancel                |    299,199 |  1.04% |
| P    | Trade (Non-Cross)           |    243,778 |  0.85% |
| F    | Add Order (MPID)            |     49,470 |  0.17% |
| Y    | Reg SHO Restriction         |      8,959 |  0.03% |
| H    | Stock Trading Action        |      8,867 |  0.03% |
| R    | Stock Directory             |      8,849 |  0.03% |
| L    | Market Participant Position |      5,936 |  0.02% |
| C    | Order Executed With Price   |      2,304 |  0.01% |
| S    | System Event                |          6 |  0.00% |
| V    | MWCB Decline Level          |          1 |  0.00% |

### Reproducing

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/itch_stats data/20190730.BX_ITCH_50
```

Decodes a full trading day (28.7M messages) at 160M msgs/s with a determinism checksum.
