# Getting started

Everything you need to build the project, run the tests, and reproduce every result.

## 1. Requirements

- **Linux**, or Windows with **WSL2** (Ubuntu)
- A **C++20 compiler** (GCC 12+ or Clang 15+), **CMake 3.25+** and **Ninja**
- **Python 3.11+**

On Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build python3-venv
```

On WSL2, keep the source and build folders on the Linux side (e.g. `~/`). Building under
`/mnt/c` is slower and can hit Windows file locks.

## 2. Build and test

```bash
# C++: library, tools, tests and benchmarks
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

# Python: builds the C++ extension too
python3 -m venv .venv && source .venv/bin/activate
pip install ".[dev]"
pytest
```

The C++ is compiled into the Python package when you run `pip install`, so rerun it after
changing C++ code.

## 3. Try it on the sample file (no download needed)

[`testdata/sample.itch`](../testdata/README.md) is a tiny hand-made trading day with two
stocks:

```bash
./build/itch_stats testdata/sample.itch      # count message types, time the parser
./build/book_replay testdata/sample.itch     # rebuild the books and check them
```

From Python:

```python
import lobpy

r = lobpy.Replayer("testdata/sample.itch")
print(r.symbols)                                   # ['AAA', 'BBB']
snaps = r.snapshots(interval_ns=1_000_000_000, depth=2)
print(lobpy.snapshots_frame(snaps, r.symbols).tail())
```

## 4. Get real data

NASDAQ publishes free sample files at
<https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH/>. This project uses five BX days:

```bash
mkdir -p data && cd data
for d in 20190730 20190830 20191030 20191230 20200130; do
  curl -LO "https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH/$d.BX_ITCH_50.gz"
  gunzip -k "$d.BX_ITCH_50.gz"
done
```

Each day is about 400–720 MB compressed and 0.8–1.6 GB uncompressed. The `data/` folder is
never committed.

## 5. Run things on a real day

| What | Command |
|---|---|
| Message counts and parser speed | `./build/itch_stats data/20190730.BX_ITCH_50` |
| Rebuild and check every book | `./build/book_replay data/20190730.BX_ITCH_50` |
| Example backtests | `python3 scripts/run_backtests.py data/20190730.BX_ITCH_50 SPY AAPL` |
| The full ML study | `python3 scripts/run_research.py data docs/research/results.json` |
| Result tables from the study | `python3 scripts/research_tables.py docs/research/results.json` |
| The charts in `docs/images/` | `python3 scripts/make_charts.py data` |
| Notebook | `pip install ".[notebook]"`, then open `notebooks/01_replay_snapshots.ipynb` |
| Dashboard | `pip install ".[dashboard]"`, then `LOB_DATA_DIR=data streamlit run dashboard/app.py` |

## 6. Benchmarks

Benchmarks need a build tuned for your CPU, and are best run pinned to one core:

```bash
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON
cmake --build build-native

export LOB_ITCH_FILE=data/20190730.BX_ITCH_50
export LOB_TSC_GHZ=3.693   # your CPU's TSC rate: dmesg | grep "tsc: Detected"
taskset -c 2 ./build-native/lob_bench --benchmark_filter=Compare \
  --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`LOB_TSC_GHZ` converts CPU timestamp ticks to nanoseconds; [benchmarks.md](benchmarks.md)
explains why it's set by hand. To get the latency histogram used in the charts, run
`taskset -c 2 ./build-native/latency_histogram data/20190730.BX_ITCH_50 > docs/images/latency_histogram.csv`.

## 7. The dashboard

```bash
pip install ".[dashboard]"
LOB_DATA_DIR=data streamlit run dashboard/app.py
```

Then open <http://localhost:8501>:
- **Choosing a moment:** pick a day, a stock and a time, and step with the buttons.
- **The model panel:** it needs the feature cache written by `scripts/run_research.py`, and
  covers the five research stocks.
- **Links:** `?day=20200130&symbol=SPY&t=10:00:11` opens a specific moment.

The GIF in the README is a screen recording. To record one automatically instead (headless
Chromium clicks through the dashboard), run this while the dashboard is running. Note that it
overwrites `docs/images/dashboard.gif`:

```bash
pip install playwright pillow && playwright install chromium
python3 scripts/record_dashboard_gif.py
```

## 8. Code style

```bash
clang-format -i cpp/include/lob/*.hpp cpp/src/*.cpp     # C++
ruff check python scripts && ruff format python scripts  # Python
```

CI checks both, builds and tests the C++ (also under AddressSanitizer and
UndefinedBehaviorSanitizer), and runs the Python tests.
