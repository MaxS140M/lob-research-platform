"""Order book features and direction labels.

Every feature at snapshot *t* is computed only from snapshots and trades at or before *t*:
rolling windows look backwards, differences are taken against the previous snapshot, and
trades are matched with ``searchsorted(..., side="right")`` on their timestamps. The
tests check this directly by changing future data and asserting that earlier features don't
move.

Prices are integers with 4 implied decimals; one tick is $0.01 = 100 units.
"""

from __future__ import annotations

from collections.abc import Sequence

import numpy as np
import pandas as pd

TICK = 100  # price units per cent

FEATURES = [
    "imbalance_l1",       # (bid size - ask size) / (bid size + ask size), top level
    "imbalance_l5",       # the same over 5 levels, each weighted by 1 / (1 + ticks from mid)
    "spread_ticks",       # ask - bid, in cents
    "microprice_offset",  # (microprice - mid) / spread: where size-weighted fair value sits
    "ofi_1",              # order flow imbalance since the previous snapshot / L1 depth
    "ofi_1s",             # the same summed over the last second
    "trade_flow_20",      # (buy - sell initiated volume) / volume, over the last 20 trades
    "volatility_5s",      # std of mid changes (bps) over the last 5 seconds
    "return_1s",          # mid change (bps) over the last second
]  # fmt: skip

DOWN, FLAT, UP = 0, 1, 2
CLASSES = ("down", "flat", "up")


def mid_price(bid: np.ndarray, ask: np.ndarray) -> np.ndarray:
    """Mid in price units, NaN where either side of the book is empty."""
    mid = (bid + ask) / 2.0
    return np.where((bid > 0) & (ask > 0), mid, np.nan)


def order_flow_imbalance(bid_px, bid_sz, ask_px, ask_sz) -> np.ndarray:
    """Cont, Kukanov & Stoikov (2014) order flow imbalance between consecutive snapshots.

    Positive when bid-side size grows or ask-side size shrinks (buying pressure). The first
    snapshot has no predecessor and gets 0.
    """
    b, bs, a, az = (np.asarray(x, dtype=float) for x in (bid_px, bid_sz, ask_px, ask_sz))
    e = np.zeros_like(bs)
    e[1:] = (
        bs[1:] * (b[1:] >= b[:-1])
        - bs[:-1] * (b[1:] <= b[:-1])
        - az[1:] * (a[1:] <= a[:-1])
        + az[:-1] * (a[1:] >= a[:-1])
    )
    return e


def trade_flow(snapshot_ts: np.ndarray, trade_ts: np.ndarray, trade_signed: np.ndarray,
               trade_volume: np.ndarray, n: int) -> np.ndarray:  # fmt: skip
    """(buy - sell initiated volume) / volume over the last ``n`` trades at or before each
    snapshot. NaN before the first trade."""
    k = np.searchsorted(trade_ts, snapshot_ts, side="right")  # trades with ts <= t
    signed = np.concatenate([[0.0], np.cumsum(trade_signed, dtype=float)])
    volume = np.concatenate([[0.0], np.cumsum(trade_volume, dtype=float)])
    lo = np.maximum(k - n, 0)
    total = volume[k] - volume[lo]
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(total > 0, (signed[k] - signed[lo]) / total, np.nan)


def _rolling_sum(x: np.ndarray, window: int) -> np.ndarray:
    """Sum of the last ``window`` values including the current one (shorter at the start)."""
    c = np.concatenate([[0.0], np.cumsum(np.nan_to_num(x), dtype=float)])
    idx = np.arange(1, len(x) + 1)
    return c[idx] - c[np.maximum(idx - window, 0)]


def book_features(ts, bid_px, bid_sz, ask_px, ask_sz, trades: pd.DataFrame | None,
                  interval_ns: int) -> pd.DataFrame:  # fmt: skip
    """Features for one symbol's snapshots on a regular time grid.

    ``bid_px`` etc. are [n, depth] arrays (best level first, 0 when absent). ``trades`` has
    columns timestamp, side (+1 buyer-initiated, -1 seller-initiated, 0 cross) and shares,
    sorted by time.
    """
    ts = np.asarray(ts)
    b1, a1 = bid_px[:, 0].astype(float), ask_px[:, 0].astype(float)
    bs1, as1 = bid_sz[:, 0].astype(float), ask_sz[:, 0].astype(float)
    mid = mid_price(b1, a1)
    steps_1s = max(1, round(1e9 / interval_ns))
    steps_5s = max(2, round(5e9 / interval_ns))

    out = pd.DataFrame(index=np.arange(len(ts)))
    out["timestamp"] = ts
    out["mid"] = mid

    with np.errstate(invalid="ignore", divide="ignore"):
        depth1 = bs1 + as1
        out["imbalance_l1"] = np.where((bs1 > 0) & (as1 > 0), (bs1 - as1) / depth1, np.nan)

        # Multi-level imbalance, weighting each level by its distance from the mid.
        dist_b = (mid[:, None] - bid_px) / TICK
        dist_a = (ask_px - mid[:, None]) / TICK
        w_b = np.where(bid_sz > 0, 1.0 / (1.0 + dist_b), 0.0)
        w_a = np.where(ask_sz > 0, 1.0 / (1.0 + dist_a), 0.0)
        wb, wa = (w_b * bid_sz).sum(axis=1), (w_a * ask_sz).sum(axis=1)
        out["imbalance_l5"] = np.where(np.isfinite(mid), (wb - wa) / (wb + wa), np.nan)

        spread = a1 - b1
        out["spread_ticks"] = np.where(np.isfinite(mid), spread / TICK, np.nan)
        micro = (b1 * as1 + a1 * bs1) / depth1
        out["microprice_offset"] = np.where(np.isfinite(mid) & (spread > 0),
                                            (micro - mid) / spread, np.nan)  # fmt: skip

        e = order_flow_imbalance(b1, bs1, a1, as1)
        mean_depth_1s = _rolling_sum(depth1, steps_1s) / np.minimum(np.arange(1, len(ts) + 1), steps_1s)
        out["ofi_1"] = np.where(depth1 > 0, e / depth1, np.nan)
        out["ofi_1s"] = np.where(mean_depth_1s > 0, _rolling_sum(e, steps_1s) / mean_depth_1s, np.nan)

    if trades is not None and len(trades):
        printable = trades[trades["side"] != 0]
        out["trade_flow_20"] = trade_flow(
            ts, printable["timestamp"].to_numpy(),
            (printable["side"] * printable["shares"]).to_numpy(),
            printable["shares"].to_numpy(), 20,
        )  # fmt: skip
    else:
        out["trade_flow_20"] = np.nan

    mid_series = pd.Series(mid)
    change_bps = mid_series.pct_change(fill_method=None) * 1e4
    out["volatility_5s"] = change_bps.rolling(steps_5s, min_periods=max(2, steps_5s // 4)).std().to_numpy()
    out["return_1s"] = (mid_series / mid_series.shift(steps_1s) - 1).to_numpy() * 1e4
    return out


def direction_labels(mid: np.ndarray, steps: int) -> np.ndarray:
    """DOWN / FLAT / UP for the mid ``steps`` snapshots ahead vs now; NaN when either is
    missing or the horizon runs past the end of the data."""
    mid = np.asarray(mid, dtype=float)
    future = np.full_like(mid, np.nan)
    if steps < len(mid):
        future[:-steps] = mid[steps:]
    diff = future - mid
    labels = np.where(diff > 0, UP, np.where(diff < 0, DOWN, FLAT)).astype(float)
    labels[~np.isfinite(diff)] = np.nan
    return labels


def build_day(replayer, symbols: Sequence[str], day: str, interval_ns: int,
              horizons_ns: Sequence[int], start_ns: int, end_ns: int) -> pd.DataFrame:  # fmt: skip
    """Features and labels for every symbol on one day, sampled every ``interval_ns``."""
    snaps = replayer.snapshots(interval_ns, depth=5, start_ns=start_ns, end_ns=end_ns)
    all_trades = replayer.trades()
    trades = pd.DataFrame({k: all_trades[k] for k in ("timestamp", "symbol", "side", "shares")})
    frames = []
    for s, symbol in enumerate(replayer.symbols):
        if symbol not in symbols:
            continue
        rows = snaps["symbol"] == s
        f = book_features(
            snaps["timestamp"][rows], snaps["bid_price"][rows], snaps["bid_size"][rows],
            snaps["ask_price"][rows], snaps["ask_size"][rows],
            trades[trades["symbol"] == s].sort_values("timestamp", kind="stable"), interval_ns,
        )  # fmt: skip
        for h in horizons_ns:
            f[f"label_{h // 10**9}s"] = direction_labels(f["mid"].to_numpy(), int(h // interval_ns))
        f.insert(0, "symbol", symbol)
        f.insert(0, "day", day)
        frames.append(f)
    return pd.concat(frames, ignore_index=True)
