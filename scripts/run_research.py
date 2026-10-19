"""The full ML study: features -> walk-forward validation -> one holdout test -> backtest.

usage: python3 scripts/run_research.py <data-dir> <output.json> [importance.png]

Development days are used for walk-forward validation only. The holdout day is used once, at
the end: models trained on all development days are scored on it and then traded on it in the
backtester. Results are written as JSON; docs/research.md is written from them.
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

import lobpy
from lobpy.backtest import HOUR, MINUTE, SECOND, BacktestConfig, Backtester, EventFeed
from lobpy.research import FEATURES, PredictionTaker, build_day, time_blocks, walk_forward
from lobpy.research.models import GradientBoosting, ImbalanceRule, Logistic, MajorityBaseline, score

DEV_DAYS = os.environ.get("LOB_DEV_DAYS", "20190730,20190830,20191030,20191230").split(",")
HOLDOUT_DAY = os.environ.get("LOB_HOLDOUT_DAY", "20200130")
SYMBOLS = ["SPY", "QQQ", "IWM", "AAPL", "MSFT"]
INTERVAL = 250_000_000  # 250 ms grid
HORIZONS = [1 * SECOND, 5 * SECOND, 30 * SECOND]
START, END = 9 * HOUR + 35 * MINUTE, 15 * HOUR + 55 * MINUTE  # skip the open; stop before the flat
MIDDAY = 12 * HOUR + 45 * MINUTE  # splits each day into am / pm validation blocks
THRESHOLDS = [0.5, 0.6]
MODELS = [MajorityBaseline, ImbalanceRule, Logistic, GradientBoosting]
# Optional cost-aware variant: model and trade only tradeable states (spread <= N ticks).
MAX_SPREAD_TICKS = float(os.environ["LOB_MAX_SPREAD_TICKS"]) if "LOB_MAX_SPREAD_TICKS" in os.environ else None


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


def load(data_dir: Path, day: str, cache_dir: Path) -> pd.DataFrame:
    cache = cache_dir / f"research_{day}.pkl"
    if cache.exists():
        return pd.read_pickle(cache)
    replayer = lobpy.Replayer(str(data_dir / f"{day}.BX_ITCH_50"), SYMBOLS)
    frame = build_day(replayer, SYMBOLS, day, INTERVAL, HORIZONS, START, END)
    frame.to_pickle(cache)
    return frame


def fit_and_score(model_cls, X_train, y_train, X_test, y_test):
    model = model_cls().fit(X_train, y_train)
    proba = model.predict_proba(X_test)
    predicted = model.predict(X_test) if hasattr(model, "predict") else None
    return model, score(y_test, proba, predicted), proba


def main() -> None:
    data_dir, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    chart_path = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    cache_dir = Path.home() / "lob-cache"
    cache_dir.mkdir(exist_ok=True)

    log("building features")
    dev = pd.concat([load(data_dir, d, cache_dir) for d in DEV_DAYS], ignore_index=True)
    holdout = load(data_dir, HOLDOUT_DAY, cache_dir)
    results: dict = {"setup": {
        "dev_days": DEV_DAYS, "holdout_day": HOLDOUT_DAY, "symbols": SYMBOLS,
        "interval_ms": INTERVAL // 1_000_000, "window": "09:35-15:55",
        "dev_rows": len(dev), "holdout_rows": len(holdout), "features": FEATURES,
        "max_spread_ticks": MAX_SPREAD_TICKS,
    }, "horizons": {}}  # fmt: skip

    for h in HORIZONS:
        label = f"label_{h // SECOND}s"
        name = f"{h // SECOND}s"
        res: dict = {}
        keep_dev = dev[label].notna()
        keep_ho = holdout[label].notna()
        if MAX_SPREAD_TICKS is not None:
            keep_dev &= dev["spread_ticks"] <= MAX_SPREAD_TICKS
            keep_ho &= holdout["spread_ticks"] <= MAX_SPREAD_TICKS
        d = dev[keep_dev].reset_index(drop=True)
        ho = holdout[keep_ho].reset_index(drop=True)
        res["class_share_dev"] = (d[label].value_counts(normalize=True).sort_index()).tolist()
        res["class_share_holdout"] = (ho[label].value_counts(normalize=True).sort_index()).tolist()

        # ---- Walk-forward on the development days ----
        folds = walk_forward(d, time_blocks(d, MIDDAY), embargo_ns=h)
        per_model = {m.name: [] for m in MODELS}
        importances = []
        for i, fold in enumerate(folds):
            Xtr, ytr = d.loc[fold.train, FEATURES], d.loc[fold.train, label].to_numpy()
            Xte, yte = d.loc[fold.test, FEATURES], d.loc[fold.test, label].to_numpy()
            for m in MODELS:
                model, s, _ = fit_and_score(m, Xtr, ytr, Xte, yte)
                per_model[m.name].append({"block": fold.test_block, "rows": len(fold.test), **s})
                if m is GradientBoosting:
                    importances.append(model.importance(FEATURES))
            log(f"{name} fold {i + 1}/{len(folds)} ({fold.test_block}) done")
        res["walk_forward"] = {
            name_: {k: float(np.mean([f[k] for f in folds_])) for k in ("accuracy", "balanced_accuracy", "log_loss")}
            for name_, folds_ in per_model.items()
        }  # fmt: skip
        res["walk_forward_folds"] = per_model
        res["importance"] = pd.concat(importances, axis=1).mean(axis=1).sort_values(ascending=False).to_dict()

        # ---- One look at the holdout day ----
        Xdev, ydev = d[FEATURES], d[label].to_numpy()
        res["holdout"] = {}
        final_lgbm = None
        for m in MODELS:
            model, s, _ = fit_and_score(m, Xdev, ydev, ho[FEATURES], ho[label].to_numpy())
            res["holdout"][m.name] = s
            if m is GradientBoosting:
                final_lgbm = model
        log(f"{name} holdout scored")

        # ---- Trade the LightGBM predictions on the holdout day ----
        res["backtest"] = {}
        replayer = lobpy.Replayer(str(data_dir / f"{HOLDOUT_DAY}.BX_ITCH_50"), SYMBOLS)
        for threshold in THRESHOLDS:
            rows = []
            for symbol in SYMBOLS:
                grid = holdout[holdout["symbol"] == symbol]
                proba = final_lgbm.predict_proba(grid[FEATURES])
                feed = EventFeed.from_replayer(replayer, symbol, depth=5, start_ns=START, end_ns=16 * HOUR)
                strategy = PredictionTaker(grid["timestamp"].to_numpy(), proba[:, 2], proba[:, 0],
                                           threshold=threshold, quantity=100, hold_ns=h,
                                           max_spread=None if MAX_SPREAD_TICKS is None
                                           else int(MAX_SPREAD_TICKS * 100))  # fmt: skip
                m = Backtester(feed, BacktestConfig(latency_ns=100_000)).run(strategy).metrics
                rows.append({"symbol": symbol, "signals": strategy.signals,
                             **{k: m[k] for k in ("net_pnl", "signal_pnl", "spread_pnl", "fees",
                                                  "traded_shares", "fill_rate")}})  # fmt: skip
            res["backtest"][str(threshold)] = rows
        log(f"{name} backtests done")
        results["horizons"][name] = res
        out_path.write_text(json.dumps(results, indent=2))

    if chart_path is not None:
        plot_importance(results, chart_path)
    log(f"wrote {out_path}")


def plot_importance(results: dict, path: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # Validated reference palette, first three categorical slots (safe all-pairs).
    colors = ["#2a78d6", "#eb6834", "#1baf7a"]
    ink, ink2, grid, surface = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
    horizons = list(results["horizons"])
    order = list(results["horizons"][horizons[0]]["importance"])[::-1]  # by 1 s importance
    fig, ax = plt.subplots(figsize=(8, 4.8), facecolor=surface)
    ax.set_facecolor(surface)
    height = 0.8 / len(horizons)
    y = np.arange(len(order))
    for i, (h, color) in enumerate(zip(horizons, colors)):
        imp = results["horizons"][h]["importance"]
        # First horizon on top within each group, matching the legend order.
        ax.barh(y - (i - (len(horizons) - 1) / 2) * height, [imp[f] * 100 for f in order],
                height=height, color=color, edgecolor=surface, linewidth=1.5, label=f"{h} horizon")  # fmt: skip
    ax.set_yticks(y, order, color=ink)
    ax.set_xlabel("Share of LightGBM split gain (%), mean over walk-forward folds", color=ink2)
    max_spread = results["setup"].get("max_spread_ticks")
    title = "Which features the model uses, by prediction horizon"
    if max_spread is not None:
        title += f"\n(only snapshots with spread ≤ {max_spread:g} ticks)"
    ax.set_title(title, loc="left", color=ink, fontweight="bold")
    ax.tick_params(colors=ink2)
    ax.grid(axis="x", color=grid, linewidth=0.8)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(grid)
    ax.legend(frameon=False, loc="lower right", labelcolor=ink)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=surface)


if __name__ == "__main__":
    main()
