"""lobpy.Replayer on testdata/sample.itch.

Same expected values as cpp/tests/test_replayer.cpp (listed in testdata/README.md), so the
Python bindings are checked against the C++ results.
"""

from pathlib import Path

import numpy as np
import pytest

import lobpy

SAMPLE = Path(__file__).resolve().parents[2] / "testdata" / "sample.itch"
SECOND = 1_000_000_000


@pytest.fixture(scope="module")
def replayer() -> lobpy.Replayer:
    return lobpy.Replayer(str(SAMPLE), ["AAA", "BBB"])


def row(second: int, symbol: int) -> int:
    return (second - 1) * 2 + symbol


def test_directory_and_time_range():
    r = lobpy.Replayer(str(SAMPLE))
    assert r.symbols == ["AAA", "BBB"]
    assert r.first_timestamp == 1 * SECOND
    assert r.last_timestamp == 15 * SECOND


def test_unknown_symbol_raises_value_error():
    with pytest.raises(ValueError, match="ZZZ"):
        lobpy.Replayer(str(SAMPLE), ["ZZZ"])


def test_bad_snapshot_arguments_raise(replayer):
    with pytest.raises(ValueError):
        replayer.snapshots(0)
    with pytest.raises(ValueError):
        replayer.snapshots(SECOND, depth=0)


def test_snapshot_shapes_and_dtypes(replayer):
    s = replayer.snapshots(SECOND, depth=2)
    assert s["timestamp"].shape == (30,)
    assert s["bid_price"].shape == (30, 2)
    assert s["timestamp"].dtype == np.int64
    assert s["symbol"].dtype == np.int32
    assert s["ask_size"].dtype == np.int64


def test_snapshots_match_hand_computed_books(replayer):
    s = replayer.snapshots(SECOND, depth=2)
    bp, bs, ap, az = s["bid_price"], s["bid_size"], s["ask_price"], s["ask_size"]

    assert s["timestamp"][row(10, 0)] == 10 * SECOND
    assert bp[row(9, 0)].tolist() == [0, 0]

    assert bp[row(10, 0)].tolist() == [100_000, 0]
    assert bs[row(10, 0)].tolist() == [100, 0]
    assert ap[row(10, 0)].tolist() == [0, 0]

    assert bp[row(11, 0)].tolist() == [100_000, 99_900]
    assert bs[row(11, 0)].tolist() == [100, 200]
    assert ap[row(11, 0)].tolist() == [100_100, 100_200]
    assert az[row(11, 0)].tolist() == [100, 50]
    assert bp[row(11, 1)].tolist() == [499_500, 0]
    assert ap[row(11, 1)].tolist() == [500_000, 0]
    assert az[row(11, 1)].tolist() == [300, 0]

    assert bp[row(12, 0)].tolist() == [99_900, 0]
    assert bs[row(12, 0)].tolist() == [150, 0]

    for t in (13, 14, 15):
        assert ap[row(t, 0)].tolist() == [100_100, 100_150]
        assert az[row(t, 0)].tolist() == [100, 80]
        assert bp[row(t, 1)].tolist() == [0, 0]
        assert ap[row(t, 1)].tolist() == [500_000, 0]


def test_snapshot_window_and_subset():
    r = lobpy.Replayer(str(SAMPLE), ["BBB"])
    s = r.snapshots(SECOND // 2, depth=1, start_ns=11 * SECOND, end_ns=13 * SECOND)
    assert s["timestamp"].tolist() == [11 * SECOND + k * SECOND // 2 for k in range(5)]
    assert s["bid_price"][:, 0].tolist() == [499_500, 499_500, 499_500, 499_500, 0]


def test_trades_match_hand_computed_executions(replayer):
    t = replayer.trades()
    assert t["timestamp"].tolist() == [
        11 * SECOND,
        12 * SECOND,
        13 * SECOND + SECOND // 2,
        14 * SECOND,
    ]
    assert t["symbol"].tolist() == [0, 0, 1, 0]
    assert t["price"].tolist() == [100_100, 99_950, 499_800, 100_000]
    assert t["shares"].tolist() == [50, 100, 25, 1_000]
    assert t["side"].tolist() == [1, -1, 1, 0]
    assert t["match_number"].tolist() == [1001, 1002, 1003, 1004]
    assert bytes(t["kind"]) == b"ECPQ"
    assert t["printable"].tolist() == [1, 1, 1, 1]


def test_arrays_are_zero_copy_views_owned_by_a_capsule(replayer):
    s = replayer.snapshots(SECOND, depth=2)
    for name, arr in s.items():
        assert not arr.flags.owndata, name  # NumPy did not allocate (or copy) this buffer
        assert type(arr.base).__name__ == "PyCapsule", name  # the C++ vector's owner
    # The buffer outlives the dict and the Replayer that produced it.
    bid = s["bid_price"]
    del s
    assert bid[row(11, 0)].tolist() == [100_000, 99_900]


def test_snapshots_frame(replayer):
    df = lobpy.snapshots_frame(replayer.snapshots(SECOND, depth=2), replayer.symbols)
    assert len(df) == 30
    aaa_11 = df[(df["symbol"] == "AAA") & (df["time"] == np.timedelta64(11, "s"))].iloc[0]
    assert aaa_11["bid_px_1"] == pytest.approx(10.00)
    assert aaa_11["ask_px_2"] == pytest.approx(10.02)
    assert aaa_11["mid"] == pytest.approx(10.005)
    assert aaa_11["spread"] == pytest.approx(0.01)
    bbb_13 = df[(df["symbol"] == "BBB") & (df["time"] == np.timedelta64(13, "s"))].iloc[0]
    assert np.isnan(bbb_13["bid_px_1"])  # empty side is NaN, not $0
    assert np.isnan(bbb_13["mid"])


def test_trades_frame(replayer):
    df = lobpy.trades_frame(replayer.trades(), replayer.symbols)
    assert df["kind"].tolist() == ["E", "C", "P", "Q"]
    assert df["symbol"].tolist() == ["AAA", "AAA", "BBB", "AAA"]
    assert df["price"].tolist() == pytest.approx([10.01, 9.995, 49.98, 10.00])
