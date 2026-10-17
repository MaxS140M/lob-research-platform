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

## Order book and matching engine

**Machine:** AMD Ryzen 5 5600X (6C/12T, TSC 3.69 GHz), WSL2 Ubuntu 24.04, GCC 13.3,
`-O3 -march=native` (CMake `Release`, `-DLOB_NATIVE=ON`). Pinned to one core with
`taskset -c 2`. Every number is the **median of 5 repetitions**, each after a warm-up.

**Latency** is the time for one operation, read with `rdtsc` and fences either side and
recorded in a histogram with one-cycle resolution. Percentiles are p50 / p99 / p99.9 in ns.
On this machine:

- **Timer overhead:** an empty timed region costs **18 ns at p50**. That's included in
  every latency below, so treat ~18 ns as the floor.
- **Timer resolution:** about **10 ns**. Under WSL2 the timestamp counter only advances in
  steps of about 37 ticks.
- **Noise:** the synthetic benchmarks repeat within 1%. The full-day replay varies by about
  ±8% between repetitions, because it is long, allocates heavily, and the host CPU's clock
  speed changes.

**Workloads:**

- **Add / Cancel:** a book of 1,000 resting 100-share orders spread over 50 price levels per
  side, one tick apart. Each op adds a passive order at a random level, or cancels a random
  live order. The book is restored, untimed, after each op so its size stays fixed.
- **Match:** the same book in `MatchingEngine`. Each op is a marketable IOC for 1–3 lots,
  which fills 1–3 resting orders at the touch, with a fill callback.
- **Full-day replay:** parse plus rebuild every book for the whole `20190730.BX_ITCH_50` day
  (28.7M messages, 8,850 books). No timers inside.
- **Real-data latency:** the book update for every order message in that day, timed
  individually and grouped by message type.

### Version history

Synthetic workloads, latency p50 / p99 / p99.9 in ns. **Timer** is that run's timer overhead
at p50. It shows the CPU clock state of the run: a run with an 18 ns timer ran about 10% faster
than one with 20 ns, so compare p50 values across rows with that in mind.

| Version | Change | Timer | Add | Cancel | Match | Full-day replay |
|---|---|---:|---:|---:|---:|---:|
| v1 | `std::map` levels, `std::list` per level, `std::unordered_map` index | 18 | 55 / 83 / 101 | 64 / 92 / 119 | 83 / 129 / 165 | 12.2 M msg/s (82 ns/msg) |
| v2 | Flat sorted vector of levels per side, best at the back | 18 | 55 / 83 / 110 | 64 / 101 / 129 | 73 / 138 / 174 | 14.6 M msg/s (69 ns/msg) |
| v3 | Order pool with free list, intrusive per-level queues | 20 | 60 / 80 / 100 | 70 / 100 / 130 | 60 / 110 / 130 | 17.1 M msg/s (59 ns/msg) |

Real ITCH data (every book update in the BX day), latency p50 / p99 / p99.9 in ns:

| Version | Add | Execute | Cancel | Delete | Replace |
|---|---:|---:|---:|---:|---:|
| v1 | 83 / 220 / 432 | 110 / 257 / 514 | 73 / 193 / 358 | 110 / 257 / 496 | 147 / 303 / 551 |
| v2 | 64 / 220 / 450 | 83 / 230 / 496 | 55 / 165 / 349 | 92 / 230 / 478 | 119 / 276 / 523 |
| v3 | 70 / 170 / 360 | 90 / 200 / 380 | 60 / 170 / 270 | 90 / 200 / 360 | 120 / 240 / 400 |

Raw Google Benchmark output for each version is in [docs/bench/](bench/).

**Correctness check for every version:** `book_replay` on the BX day must report 0 anomalies,
no locked or crossed books, and the same top-of-book digest after every one of the 28.7M
messages. For v1 that digest is `da514e7cf954bd6a`.

### What each version changed

**v2: flat price levels.** Each side's levels are kept in one sorted `std::vector`, ordered
worst to best, so the best level is `back()`. Finding a level scans back from the touch and
switches to binary search after 16 steps, so a deep book stays O(log n).

- **Real data:** clear gains. Add p50 dropped from 83 to 64 ns, execute from 110 to 83 ns,
  and full-day throughput rose 20%. Real books are shallow and most updates are within a
  few levels of the touch.
- **Synthetic add / cancel:** flat. That workload picks one of 50 levels uniformly at random,
  which is the worst case for a scan from the touch. Most of each operation's cost is still
  the `malloc` for the list node and the `unordered_map` lookup.

**v3: order pool and intrusive lists.** Orders live in one contiguous `std::vector<Node>` with
a free list, so after warm-up an add never calls `malloc`. Each level's FIFO queue is a doubly
linked list threaded through the pool by 32-bit `prev`/`next` indices. A level shrinks to a
20-byte trivially copyable struct (`price, quantity, count, head, tail`), so inserting a level
mid-vector is a `memmove`.

- **Tails:** the gain shows up at the tail rather than the median. On real data, add p99.9
  dropped from 450 to 360 ns and execute p99.9 from 496 to 380 ns.
- **Throughput and matching:** full-day replay rose to 17.1 M msg/s, and synthetic match fell
  from 73 / 138 to 60 / 110 ns. Allocator calls are exactly the occasional slow path that
  shows up at p99.

### Where v1 spends its time

`perf record` on the full-day replay, by source line:

| Cost | Share | Cause |
|---|---:|---|
| `std::map` tree walk (`stl_tree.h`, `std::less`) | ~20% | finding the price level on every add and erase |
| `std::unordered_map` (hashing, bucket chains) | ~25% | order id lookup on every message |
| `malloc` / `free` | ~7% | one list node and one hash node per order |

### Reproducing

```bash
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON
cmake --build build-native
LOB_ITCH_FILE=data/20190730.BX_ITCH_50 taskset -c 2 ./build-native/lob_bench \
  --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_out=bench.json --benchmark_out_format=json
python3 scripts/bench_summary.py bench.json v1
./build-native/book_replay data/20190730.BX_ITCH_50
```
