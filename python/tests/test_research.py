"""Research layer tests: feature formulas, the no-lookahead guarantee, splits and models."""

import numpy as np
import pandas as pd
import pytest
from test_backtest import make_feed

from lobpy.backtest import BUY, BacktestConfig, Backtester
from lobpy.research import (
    DOWN,
    FEATURES,
    FLAT,
    UP,
    PredictionTaker,
    book_features,
    direction_labels,
    time_blocks,
    walk_forward,
)
from lobpy.research.features import order_flow_imbalance, trade_flow

INTERVAL = 250_000_000  # 250 ms


def random_book(n: int, seed: int):
    """A plausible random book: mid random-walks in ticks, sizes vary, depth 5."""
    rng = np.random.default_rng(seed)
    mid_ticks = 10_000 + np.cumsum(rng.integers(-1, 2, n))
    half_spread = rng.integers(1, 3, n)
    bid = (mid_ticks - half_spread)[:, None] - np.arange(5)[None, :]
    ask = (mid_ticks + half_spread)[:, None] + np.arange(5)[None, :]
    ts = np.arange(n) * INTERVAL
    sizes = lambda: rng.integers(1, 10, (n, 5)) * 100  # noqa: E731
    trades = pd.DataFrame(
        {
            "timestamp": np.sort(rng.integers(0, n * INTERVAL, 3 * n)),
            "side": rng.choice([-1, 1], 3 * n),
            "shares": rng.integers(1, 5, 3 * n) * 100,
        }
    )
    return ts, bid * 100, sizes(), ask * 100, sizes(), trades


# ---- Feature formulas ----------------------------------------------------------------------


def test_order_flow_imbalance_matches_definition():
    # Step 1: bid price up (new bid 200 counts fully), ask unchanged (size 300 -> 100: -200 asks
    # left, i.e. +200 to OFI). Step 2: bid price down (the old 200 is lost), ask price down
    # (new ask size 50 adds selling pressure).
    bid = np.array([100, 101, 100])
    bid_sz = np.array([500, 200, 400])
    ask = np.array([103, 103, 102])
    ask_sz = np.array([300, 100, 50])
    e = order_flow_imbalance(bid, bid_sz, ask, ask_sz)
    assert e.tolist() == [0, 200 - 100 + 300, -200 - 50]


def test_trade_flow_uses_only_trades_at_or_before_each_snapshot():
    trade_ts = np.array([10, 20, 30])
    side = np.array([1, -1, 1])
    shares = np.array([100, 50, 30])
    flow = trade_flow(np.array([5, 20, 25, 30]), trade_ts, side * shares, shares, n=2)
    assert np.isnan(flow[0])
    assert flow[1:].tolist() == pytest.approx([50 / 150, 50 / 150, -20 / 80])


def test_direction_labels():
    mid = np.array([1.0, 2.0, 2.0, 1.0, np.nan, 3.0])
    labels = direction_labels(mid, steps=1)
    assert labels[:3].tolist() == [UP, FLAT, DOWN]
    assert np.isnan(labels[3:]).all()  # future missing, or no future at all


def test_features_have_expected_ranges():
    ts, bp, bs, ap, az, trades = random_book(2_000, seed=1)
    f = book_features(ts, bp, bs, ap, az, trades, INTERVAL)
    assert list(f.columns[2:]) == FEATURES
    assert f["imbalance_l1"].between(-1, 1).all()
    assert f["imbalance_l5"].between(-1, 1).all()
    assert f["microprice_offset"].between(-0.5, 0.5).all()
    assert (f["spread_ticks"] >= 2).all()
    assert f["trade_flow_20"].dropna().between(-1, 1).all()


# ---- No lookahead ------------------------------------------------------------------------------


@pytest.mark.parametrize("cut", [100, 777, 1_500])
def test_no_feature_depends_on_data_after_its_timestamp(cut):
    """Scramble every snapshot and trade after row ``cut``; features up to ``cut`` must not move."""
    ts, bp, bs, ap, az, trades = random_book(2_000, seed=2)
    before = book_features(ts, bp, bs, ap, az, trades, INTERVAL)

    _, bp2, bs2, ap2, az2, trades2 = random_book(2_000, seed=99)
    bp, bs, ap, az = (x.copy() for x in (bp, bs, ap, az))
    bp[cut + 1 :], bs[cut + 1 :], ap[cut + 1 :], az[cut + 1 :] = (
        bp2[cut + 1 :],
        bs2[cut + 1 :],
        ap2[cut + 1 :],
        az2[cut + 1 :],
    )
    future = trades2[trades2["timestamp"] > ts[cut]]
    trades = pd.concat([trades[trades["timestamp"] <= ts[cut]], future]).sort_values("timestamp")
    after = book_features(ts, bp, bs, ap, az, trades, INTERVAL)

    pd.testing.assert_frame_equal(before.iloc[: cut + 1], after.iloc[: cut + 1])
    # Sanity: the scramble did change later rows, so the test has teeth.
    assert not before.iloc[cut + 1 :].equals(after.iloc[cut + 1 :])


def test_labels_do_look_ahead_and_are_kept_separate():
    ts, bp, bs, ap, az, trades = random_book(500, seed=3)
    f = book_features(ts, bp, bs, ap, az, trades, INTERVAL)
    assert not any(c.startswith("label") for c in f.columns)  # features never include labels


# ---- Walk-forward splits -----------------------------------------------------------------------


def test_walk_forward_is_time_ordered_with_an_embargo():
    days = ["20190730", "20190830", "20191030"]
    ts = np.arange(0, 100) * 10
    frame = pd.DataFrame({"day": np.repeat(days, len(ts)), "timestamp": np.tile(ts, len(days))})
    blocks = time_blocks(frame, split_ns=500)
    folds = walk_forward(frame, blocks, embargo_ns=50)

    assert [f.test_block for f in folds] == [
        "20190730 pm",
        "20190830 am",
        "20190830 pm",
        "20191030 am",
        "20191030 pm",
    ]
    for fold in folds:
        assert len(np.intersect1d(fold.train, fold.test)) == 0
        assert fold.train.max() < fold.test.min()  # training is strictly earlier
        train, test = frame.iloc[fold.train], frame.iloc[fold.test]
        same_day = train[train["day"] == test["day"].iloc[0]]
        if len(same_day):
            gap = test["timestamp"].min() - same_day["timestamp"].max()
            assert gap > 50  # strictly more than the embargo
    # The first fold tests the afternoon of day 1 (from 500): training stops before 450.
    assert frame.iloc[folds[0].train]["timestamp"].max() == 440


def test_walk_forward_rejects_shuffled_rows():
    frame = pd.DataFrame({"day": ["20190830", "20190730"], "timestamp": [0, 0]})
    with pytest.raises(ValueError):
        walk_forward(frame, time_blocks(frame, 1), embargo_ns=0)


# ---- Models --------------------------------------------------------------------------------------


def synthetic_dataset(n: int = 6_000, seed: int = 4):
    """Labels driven by imbalance plus noise: every model should beat the majority baseline."""
    rng = np.random.default_rng(seed)
    X = pd.DataFrame(rng.normal(size=(n, len(FEATURES))), columns=FEATURES)
    X["imbalance_l1"] = rng.uniform(-1, 1, n)
    signal = X["imbalance_l1"] + rng.normal(scale=0.4, size=n)
    y = np.where(signal > 0.3, UP, np.where(signal < -0.3, DOWN, FLAT)).astype(float)
    return X, y


def test_models_beat_the_majority_baseline_on_a_planted_signal():
    pytest.importorskip("sklearn")
    pytest.importorskip("lightgbm")
    from lobpy.research.models import (
        GradientBoosting,
        ImbalanceRule,
        Logistic,
        MajorityBaseline,
        score,
    )

    X, y = synthetic_dataset()
    train, test = slice(0, 4_000), slice(4_000, None)
    results = {}
    small = GradientBoosting(min_child_samples=50)  # the default suits millions of rows
    for model in (MajorityBaseline(), ImbalanceRule(), Logistic(), small):
        model.fit(X[train], y[train])
        proba = model.predict_proba(X[test])
        assert proba.shape == (2_000, 3)
        assert np.allclose(proba.sum(axis=1), 1)
        results[model.name] = score(y[test], proba)
    base = results["Majority class"]
    for name in ("Imbalance rule", "Logistic regression", "LightGBM"):
        assert results[name]["log_loss"] < base["log_loss"], name
        assert results[name]["balanced_accuracy"] > base["balanced_accuracy"] + 0.2, name


# ---- Trading predictions ---------------------------------------------------------------------


def test_prediction_strategy_never_uses_a_later_prediction():
    rows = [
        {"t": 5, "bids": [(99, 1_000)], "asks": [(101, 1_000)]},
        {"t": 10, "bids": [(99, 1_000)], "asks": [(101, 1_000)]},
        {"t": 20, "bids": [(99, 1_000)], "asks": [(101, 1_000)]},
    ]
    # One prediction, stamped t = 10: strongly up. The event at t = 5 must not act on it.
    strategy = PredictionTaker(
        np.array([10]), np.array([0.9]), np.array([0.05]), threshold=0.6, hold_ns=10**12
    )
    cfg = BacktestConfig(latency_ns=1, flatten_at_ns=None, depth=2)
    result = Backtester(make_feed(rows), cfg).run(strategy)
    fills = result.fills
    assert fills["side"].tolist() == [BUY]
    assert [t.value for t in fills["time"]] == [11]  # sent at the t = 10 event, not t = 5


def test_prediction_strategy_skips_wide_spreads():
    rows = [
        {"t": 10, "bids": [(90, 1_000)], "asks": [(110, 1_000)]},  # 20-unit spread: too wide
        {"t": 20, "bids": [(99, 1_000)], "asks": [(101, 1_000)]},  # 2 units: fine
        {"t": 30, "bids": [(99, 1_000)], "asks": [(101, 1_000)]},
    ]
    strategy = PredictionTaker(
        np.array([0]),
        np.array([0.9]),
        np.array([0.05]),
        threshold=0.6,
        hold_ns=10**12,
        max_spread=5,
    )
    result = Backtester(
        make_feed(rows), BacktestConfig(latency_ns=1, flatten_at_ns=None, depth=2)
    ).run(strategy)
    assert [t.value for t in result.fills["time"]] == [21]  # entered at t = 20, not t = 10
