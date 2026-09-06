# Project Notes

I’m working on an order book research platform to explore market data and better understand how order book activity relates to price movements and liquidity.

I’m looking for an outcome that makes it easy to study historical and live order book data, test research ideas, and clearly present useful findings. The goal is a practical tool that helps me make more informed decisions based on the data.

## Where things stand

- **v0.1.0:** project skeleton. CMake build, GoogleTest, the `lobpy` Python package, and CI on every PR.
- **v0.2.0:** ITCH 5.0 parser. It reads a full NASDAQ BX day (28.7M messages, 837 MB) in 0.18 s with every field decoded, about 6.3 ns per message.
- **Next:** the order book. It rebuilds every resting order from the add, execute, cancel, delete and replace messages.

## How to run it

From an Ubuntu (WSL) terminal:

```bash
cd /mnt/c/Users/maxsl/Desktop/OrderBookResearchPlatform
cmake -S . -B ~/build/lob -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ~/build/lob
ctest --test-dir ~/build/lob --output-on-failure
~/build/lob/itch_stats ~/lob-data/20190730.BX_ITCH_50
```

- Keep the build folder in `~/build/lob`, not `build/` on `C:`. Windows locks files during the GoogleTest download and the build fails.
- The data lives in `~/lob-data` inside WSL, because reading from `C:` is much slower. The ITCH spec PDF is there too, and `spec.txt` is a searchable text copy.
- To time a "from disk" run, empty the file cache first with `echo 3 > /proc/sys/vm/drop_caches`.

## How the data works

- An ITCH file is raw bytes, not text. Each message is a 2-byte length, then the message. The first byte of the message is its type.
- Every number is big-endian, so the bytes have to be swapped on a normal PC.
- Prices are integers with 4 implied decimals: `1823500` means $182.35. Never use floating point for prices, because 0.1 + 0.2 ≠ 0.3 in floating point and money can't be off by a rounding error.
- Timestamps are 6 bytes of nanoseconds since midnight.
- The messages that matter for the book:

| Type | Message | Effect on the book |
|---|---|---|
| `A` / `F` | Add order | A new resting order at a price |
| `E` / `C` | Executed | Shares removed because they traded |
| `X` | Cancel | Some shares removed, the order stays |
| `D` | Delete | The whole order removed |
| `U` | Replace | The old order removed, a new one added on the same side |
| `P` | Trade | A hidden order traded; the visible book doesn't change |

## What the first day of data showed

From `20190730.BX_ITCH_50`:

- **Most orders never trade.** Adds (36.8%) and deletes (35.4%) make up about 72% of all messages, but executions are only about 2.4%. Most of the book is orders being posted and pulled, which is what market makers do as they keep re-quoting.
- **Session times:** messages start at 03:06, the system opens at 07:00, the market opens at 09:30 and closes at 16:00, the system closes at 19:00 and messages end at 19:05. Pre-market and after-hours data needs separate treatment, because liquidity is much thinner then.
- **Retail Price Improvement messages (`N`) are 16% of the day** on BX. They're specific to this exchange and don't affect the visible book.
- **Timestamps never go backwards** across the whole file, so the replay can trust message order.

## How the ML will work

The idea: the shape of the book right now says a little about which way the price will move in the next few seconds.

1. **Snapshots:** while the C++ engine replays the day, record the book state every N updates or every second.
2. **Features:** turn each snapshot into numbers:
   - **Order book imbalance** at the best price: (bid size − ask size) / (bid size + ask size). This is usually the strongest single feature.
   - **Order flow imbalance:** net change in bid versus ask size over the last N events.
   - **Spread and microprice** (the mid-price weighted by size).
   - **Depth imbalance at levels 2–5**, recent trade direction, short-term volatility and time of day.
3. **Label:** did the mid-price go up, down or stay the same over the next k events? That makes it a 3-class problem.
4. **Model:** LightGBM, gradient-boosted trees. It's fast and strong on tabular data, and it shows which features it actually uses.
5. **Backtest:** trade only on confident predictions, and pay the spread, fees and a latency delay. The question is whether the signal survives costs, not just whether it predicts.

## Pitfalls to avoid

- **Splitting data randomly.** Neighbouring snapshots are almost identical, so a random split leaks the future into training. Always split by time: train on earlier days, test on later ones.
- **Lookahead:** every feature may only use information available at that exact moment.
- **Trusting accuracy:** if most labels are "unchanged", a model that always says "unchanged" looks good. Use log-loss and AUC, and compare against an imbalance-only baseline.
- **Ignoring costs:** a model that's right 55% of the time can still lose money once it pays the spread.
- **An incorrect book:** one mishandled cancel or replace corrupts every feature after it, so the book needs to be tested hard before any ML.
- **Overfitting to one day:** one day is one market regime. Results need to hold across several days.

## Data plan

- One day is enough to build and test the book, but not to train and evaluate a model.
- The NASDAQ BX folder has about 7 days available (May, July, August, October and December 2019, and January 2020). Download 4–5 of them before the ML work.
- Full NASDAQ days are 5–12 GB compressed. Only use one once everything works on BX.

## Open questions

- **Live data:** the ITCH files are historical. A live order book feed needs a paid data source or a broker API. Decide whether "live" means real-time data or replaying history as if it were live.
- **Which stocks to study:** liquid names (tight spreads, lots of updates) or a mix? The signal probably behaves differently in each.
- **Snapshot frequency:** every event, every N events or every second? This affects dataset size and what the model can learn.
- **Presenting findings:** notebooks, a written report, a dashboard, or some combination.
