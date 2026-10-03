"""Convert Replayer results (dicts of NumPy arrays) into pandas DataFrames.

The C++ layer keeps prices as integers with 4 implied decimals. These helpers are where they
become dollars, for research convenience. Do any exact price comparisons on the raw arrays.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence

import numpy as np
import pandas as pd

from ._lob import PRICE_SCALE


def _time_index(timestamp_ns: np.ndarray) -> pd.TimedeltaIndex:
    return pd.to_timedelta(timestamp_ns, unit="ns")


def snapshots_frame(snapshots: Mapping[str, np.ndarray], symbols: Sequence[str]) -> pd.DataFrame:
    """One row per (sample time, symbol), with level columns bid_px_1..N, bid_sz_1..N, etc.

    Adds ``mid`` and ``spread`` (in dollars), which are NaN when either side is empty.
    """
    depth = snapshots["bid_price"].shape[1]
    columns: dict[str, np.ndarray] = {
        "time": _time_index(snapshots["timestamp"]),
        "symbol": pd.Categorical.from_codes(snapshots["symbol"], categories=list(symbols)),
    }
    for side in ("bid", "ask"):
        prices = snapshots[f"{side}_price"]
        sizes = snapshots[f"{side}_size"]
        for level in range(depth):
            px = prices[:, level].astype(np.float64) / PRICE_SCALE
            columns[f"{side}_px_{level + 1}"] = np.where(sizes[:, level] > 0, px, np.nan)
            columns[f"{side}_sz_{level + 1}"] = sizes[:, level]
    frame = pd.DataFrame(columns)
    frame["mid"] = (frame["bid_px_1"] + frame["ask_px_1"]) / 2
    frame["spread"] = frame["ask_px_1"] - frame["bid_px_1"]
    return frame


def trades_frame(trades: Mapping[str, np.ndarray], symbols: Sequence[str]) -> pd.DataFrame:
    """One row per execution, with price in dollars and kind as a one-letter string."""
    return pd.DataFrame(
        {
            "time": _time_index(trades["timestamp"]),
            "symbol": pd.Categorical.from_codes(trades["symbol"], categories=list(symbols)),
            "price": trades["price"].astype(np.float64) / PRICE_SCALE,
            "shares": trades["shares"],
            "side": trades["side"],
            "match_number": trades["match_number"],
            "kind": trades["kind"].view("S1").astype(str),
            "printable": trades["printable"].astype(bool),
        }
    )
