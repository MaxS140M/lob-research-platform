"""Walk-forward splits with an embargo.

Rows are never shuffled. The development period is cut into consecutive time blocks; fold i
trains on every block before block i+1 (an expanding window) and tests on block i+1. An
**embargo** drops training rows less than or exactly ``embargo_ns`` before the start of
the test block on the same day. With an embargo at least as long as the label horizon, no
training label can depend on prices inside the test block.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd


@dataclass(frozen=True)
class Fold:
    train: np.ndarray  # row positions
    test: np.ndarray
    test_block: str  # e.g. "20190830 am"


def time_blocks(frame: pd.DataFrame, split_ns: int) -> pd.Series:
    """Labels each row with its block: the day, split into "am" / "pm" at ``split_ns``."""
    half = np.where(frame["timestamp"].to_numpy() < split_ns, "am", "pm")
    return frame["day"].astype(str) + " " + half


def walk_forward(
    frame: pd.DataFrame, blocks: pd.Series, embargo_ns: int, min_train_blocks: int = 1
) -> list[Fold]:
    """Expanding-window walk-forward folds over ``blocks`` (in time order)."""
    order = list(dict.fromkeys(blocks))  # blocks in first-appearance (time) order
    if order != sorted(order):
        raise ValueError("rows must be in time order (day, then timestamp)")
    position = blocks.map({b: i for i, b in enumerate(order)}).to_numpy()
    day = frame["day"].to_numpy()
    ts = frame["timestamp"].to_numpy()

    folds = []
    for k in range(min_train_blocks, len(order)):
        test = np.flatnonzero(position == k)
        train_mask = position < k
        test_day = day[test[0]]
        test_start = ts[test].min()
        # Embargo: a training row on the test day whose label horizon reaches the test block.
        train_mask &= ~((day == test_day) & (ts >= test_start - embargo_ns))
        folds.append(Fold(np.flatnonzero(train_mask), test, order[k]))
    return folds
