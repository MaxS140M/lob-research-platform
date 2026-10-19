"""Make the charts in docs/images/.

usage: python3 scripts/make_charts.py <data-dir> [out-dir]

Needs, from earlier steps (see docs/getting-started.md):
- docs/images/latency_histogram.csv     from the latency_histogram tool
- docs/research/results.json            from scripts/run_research.py
- the feature cache (~/lob-cache)       written by scripts/run_research.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

import lobpy  # noqa: E402
from lobpy.backtest import HOUR, MINUTE, SECOND, BacktestConfig, Backtester, EventFeed  # noqa: E402
from lobpy.research import FEATURES, PredictionTaker  # noqa: E402
from lobpy.research.models import GradientBoosting  # noqa: E402

# Validated reference palette (dataviz skill): the first categorical slots, plus text colours.
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

plt.rcParams.update(
    {
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID,
        "axes.labelcolor": INK_2,
        "xtick.color": INK_2,
        "ytick.color": INK_2,
        "text.color": INK,
        "axes.grid": True,
        "grid.color": GRID,
        "grid.linewidth": 0.8,
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "font.size": 10,
        "axes.titlesize": 12,
        "axes.titleweight": "bold",
        "axes.titlelocation": "left",
        "legend.frameon": False,
    }
)

ROOT = Path(__file__).resolve().parents[1]
SYMBOLS = ["SPY", "QQQ", "IWM", "AAPL", "MSFT"]
DEV_DAYS = ["20190730", "20190830", "20191030", "20191230"]
HOLDOUT_DAY = "20200130"
START = 9 * HOUR + 35 * MINUTE


def save(fig, path: Path) -> None:
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"wrote {path}")


def hours_axis(ax, index_ns, step_ns: int) -> None:
    """Label an x-axis of ns-since-midnight values as HH:MM, with a tick every ``step_ns``."""
    first = -(-int(index_ns[0]) // step_ns) * step_ns  # first whole step at or after the start
    ticks = np.arange(first, int(index_ns[-1]) + 1, step_ns)
    ax.set_xticks(ticks, [f"{t // HOUR:02d}:{t % HOUR // MINUTE:02d}" for t in ticks])
    ax.set_xlim(index_ns[0], index_ns[-1])


def money(value: float) -> str:
    """$1,234 / −$1,234, with a proper minus sign."""
    return f"{'−' if value < 0 else ''}${abs(value):,.0f}"


# ---- 1. The order book through time ---------------------------------------------------------


def chart_book(data_dir: Path, out: Path) -> None:
    """Best bid and ask for SPY over ten minutes, with every trade marked by its aggressor."""
    start, end = 10 * HOUR, 10 * HOUR + 10 * MINUTE
    r = lobpy.Replayer(str(data_dir / "20190730.BX_ITCH_50"), ["SPY"])
    s = r.snapshots(SECOND // 10, depth=1, start_ns=start, end_ns=end)
    t = r.trades()
    in_window = (t["timestamp"] >= start) & (t["timestamp"] <= end) & (t["side"] != 0)
    ts, price, side = t["timestamp"][in_window], t["price"][in_window] / 1e4, t["side"][in_window]

    fig, ax = plt.subplots(figsize=(9, 4.2))
    x = s["timestamp"]
    bid = np.where(s["bid_size"][:, 0] > 0, s["bid_price"][:, 0] / 1e4, np.nan)
    ask = np.where(s["ask_size"][:, 0] > 0, s["ask_price"][:, 0] / 1e4, np.nan)
    ax.step(x, ask, where="post", color=ORANGE, linewidth=1.2, label="Best ask")
    ax.step(x, bid, where="post", color=BLUE, linewidth=1.2, label="Best bid")
    ax.scatter(
        ts[side > 0],
        price[side > 0],
        marker="^",
        s=22,
        color=INK,
        linewidths=0,
        label="Trade, buyer-initiated",
        zorder=3,
    )
    ax.scatter(
        ts[side < 0],
        price[side < 0],
        marker="v",
        s=22,
        color=INK_2,
        linewidths=0,
        label="Trade, seller-initiated",
        zorder=3,
    )
    ax.set_title("SPY on NASDAQ BX, 30 July 2019, 10:00–10:10")
    ax.set_ylabel("Price ($)")
    hours_axis(ax, x, 2 * MINUTE)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.1), ncols=4)
    save(fig, out / "order_book_spy.png")


# ---- 2. Latency before and after optimisation -------------------------------------------------


def chart_latency(out: Path) -> None:
    """Distribution of book-update latency on the real day: first version vs current."""
    path = out / "latency_histogram.csv"
    lines = path.read_text().splitlines()
    ghz = float(lines[0].split("=")[1])
    h = pd.read_csv(path, comment="#")
    ns = h["cycles"].to_numpy() / ghz

    def pct(counts: np.ndarray, q: float) -> float:
        c = np.cumsum(counts)
        return float(ns[np.searchsorted(c, q * c[-1])])

    bins = np.arange(0, 610, 10)  # 10 ns bins: about the timer's resolution
    fig, ax = plt.subplots(figsize=(9, 4.2))
    for col, color, name in (
        ("v1", ORANGE, "First version (std::map, std::list)"),
        ("current", BLUE, "Optimised"),
    ):
        counts = h[col].to_numpy()
        share, _ = np.histogram(np.minimum(ns, bins[-1] - 1), bins=bins, weights=counts)
        share = share / counts.sum() * 100
        p50, p99 = pct(counts, 0.5), pct(counts, 0.99)
        ax.stairs(
            share,
            bins,
            color=color,
            linewidth=2,
            label=f"{name}: p50 {p50:.0f} ns, p99 {p99:.0f} ns",
        )
    ax.set_title("Time to apply one book update, every update of a real trading day")
    ax.set_xlabel("Latency (ns), including ~20 ns of timer overhead")
    ax.set_ylabel("Share of updates (%)")
    ax.set_xlim(0, 600)
    ax.legend(loc="upper right")
    save(fig, out / "latency.png")


# ---- 3. Model accuracy by horizon ---------------------------------------------------------------


def chart_accuracy(results: dict, out: Path) -> None:
    """Balanced accuracy on the holdout day, by prediction horizon, against chance."""
    models = [("Imbalance rule", AQUA), ("Logistic regression", ORANGE), ("LightGBM", BLUE)]
    horizons = list(results["horizons"])
    x = np.arange(len(horizons))
    width = 0.26
    fig, ax = plt.subplots(figsize=(8, 4.2))
    for i, (name, color) in enumerate(models):
        values = [
            results["horizons"][h]["holdout"][name]["balanced_accuracy"] * 100 for h in horizons
        ]
        bars = ax.bar(
            x + (i - 1) * width,
            values,
            width=width,
            color=color,
            edgecolor=SURFACE,
            linewidth=2,
            label=name,
        )
        if name == "LightGBM":
            ax.bar_label(bars, fmt="%.1f%%", padding=3, color=INK, fontsize=9)
    ax.axhline(100 / 3, color=INK_2, linestyle="--", linewidth=1)
    ax.text(-0.45, 100 / 3 + 0.3, "chance (33.3%)", ha="left", va="bottom", color=INK_2)
    ax.set_xticks(x, [f"Next {h}" for h in horizons])
    ax.set_ylim(25, 50)
    ax.set_ylabel("Balanced accuracy (%)")
    ax.set_title("Accuracy on a day never used in development")
    ax.grid(axis="x", visible=False)
    ax.legend(loc="upper right", ncols=3)
    save(fig, out / "model_accuracy.png")


# ---- 4. PnL before and after costs ---------------------------------------------------------------


def chart_pnl(data_dir: Path, cache: Path, out: Path) -> None:
    """Cumulative PnL of the tight-spread 5 s strategy on the holdout day, before and after costs.

    "After costs" is the backtest's real equity. "Before costs" adds back what was paid to cross
    the spread and in fees, i.e. as if every fill had happened at the mid with no fee.
    """
    dev = pd.concat([pd.read_pickle(cache / f"research_{d}.pkl") for d in DEV_DAYS])
    ho = pd.read_pickle(cache / f"research_{HOLDOUT_DAY}.pkl")
    keep = dev["label_5s"].notna() & (dev["spread_ticks"] <= 2)
    model = GradientBoosting().fit(dev.loc[keep, FEATURES], dev.loc[keep, "label_5s"].to_numpy())

    replayer = lobpy.Replayer(str(data_dir / f"{HOLDOUT_DAY}.BX_ITCH_50"), SYMBOLS)
    grid = np.arange(START, 16 * HOUR + 1, MINUTE)
    after, before = np.zeros(len(grid)), np.zeros(len(grid))
    for symbol in SYMBOLS:
        rows = ho[ho["symbol"] == symbol]
        p = model.predict_proba(rows[FEATURES])
        feed = EventFeed.from_replayer(replayer, symbol, depth=5, start_ns=START, end_ns=16 * HOUR)
        strategy = PredictionTaker(
            rows["timestamp"].to_numpy(),
            p[:, 2],
            p[:, 0],
            threshold=0.5,
            hold_ns=5 * SECOND,
            max_spread=200,
        )
        res = Backtester(feed, BacktestConfig(latency_ns=100_000)).run(strategy)

        net = res.equity.groupby(level=0).last()
        f = res.fills
        cost = (f["side"] * (f["price"] - f["mid"]) * f["quantity"] + f["fee"]).cumsum()
        cost = cost.groupby(f["time"]).last()
        net_on_grid = net.reindex(pd.to_timedelta(grid, unit="ns"), method="ffill").fillna(0)
        cost_on_grid = cost.reindex(pd.to_timedelta(grid, unit="ns"), method="ffill").fillna(0)
        after += net_on_grid.to_numpy()
        before += (net_on_grid + cost_on_grid).to_numpy()

    fig, ax = plt.subplots(figsize=(9, 4.2))
    ax.plot(
        grid, before, color=BLUE, linewidth=2, label="Before costs (filled at the mid, no fees)"
    )
    ax.plot(
        grid, after, color=ORANGE, linewidth=2, label="After costs (paying the spread and fees)"
    )
    ax.axhline(0, color=INK_2, linewidth=0.8)
    for series in (before, after):
        ax.annotate(
            money(series[-1]),
            (grid[-1], series[-1]),
            xytext=(4, 0),
            textcoords="offset points",
            va="center",
            color=INK,
        )
    ax.set_title("Trading the 5 s prediction on the holdout day (5 stocks, tight spreads only)")
    ax.set_ylabel("Cumulative PnL ($)")
    hours_axis(ax, grid, HOUR)
    ax.legend(loc="lower left")
    save(fig, out / "pnl_costs.png")


def main() -> None:
    data_dir = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "docs" / "images"
    out.mkdir(parents=True, exist_ok=True)
    results = json.loads((ROOT / "docs" / "research" / "results.json").read_text())
    chart_book(data_dir, out)
    chart_latency(out)
    chart_accuracy(results, out)
    chart_pnl(data_dir, Path.home() / "lob-cache", out)


if __name__ == "__main__":
    main()
