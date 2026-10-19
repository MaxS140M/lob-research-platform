"""Where does the model's confidence come from? The spread at confident predictions.

Trains LightGBM on the development days (feature cache written by run_research.py) and, on
the holdout day, compares the spread at confident predictions (p(up) or p(down) > 0.6) with
the spread overall. Produces the "Where does the confidence come from?" table in
docs/research-report.md.

usage: python3 scripts/research_diagnose.py [cache-dir]
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

from lobpy.research import DOWN, FEATURES, UP
from lobpy.research.models import GradientBoosting

DEV_DAYS = ["20190730", "20190830", "20191030", "20191230"]
HOLDOUT_DAY = "20200130"


def main() -> None:
    cache = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "lob-cache"
    dev = pd.concat(
        [pd.read_pickle(cache / f"research_{d}.pkl") for d in DEV_DAYS], ignore_index=True
    )
    ho = pd.read_pickle(cache / f"research_{HOLDOUT_DAY}.pkl")
    spread = ho["spread_ticks"].to_numpy()

    print(
        "| Horizon | Confident | Median spread when confident | Median spread overall "
        "| Confident calls at <= 2 ticks | Direction right when confident |"
    )
    print("|---|---:|---:|---:|---:|---:|")
    for label in ("label_1s", "label_5s", "label_30s"):
        d = dev[dev[label].notna()]
        p = GradientBoosting().fit(d[FEATURES], d[label].to_numpy()).predict_proba(ho[FEATURES])
        confident = np.maximum(p[:, DOWN], p[:, UP]) > 0.6
        y = ho[label].to_numpy()
        called = np.where(p[:, UP] > p[:, DOWN], UP, DOWN)
        directional = confident & np.isin(y, (DOWN, UP))
        print(
            f"| {label[6:]} | {confident.mean():.1%} | {np.median(spread[confident]):.0f} ticks "
            f"| {np.median(spread):.0f} ticks | {(spread[confident] <= 2).mean():.1%} "
            f"| {(called[directional] == y[directional]).mean():.1%} |"
        )


if __name__ == "__main__":
    main()
