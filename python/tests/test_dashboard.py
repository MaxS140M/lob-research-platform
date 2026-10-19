"""The dashboard renders and responds, using the tiny sample file (no market data needed)."""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def app(monkeypatch, tmp_path):
    testing = pytest.importorskip("streamlit.testing.v1")
    monkeypatch.setenv("LOB_DATA_DIR", str(tmp_path))  # no real days: only the sample file
    monkeypatch.setenv("LOB_CACHE_DIR", str(tmp_path))
    at = testing.AppTest.from_file(str(ROOT / "dashboard" / "app.py"), default_timeout=60)
    at.run()
    return at


def metrics(at) -> dict[str, str]:
    return {m.label: m.value for m in at.metric}


def test_dashboard_shows_the_book_at_the_end_of_the_sample_day(app):
    assert not app.exception
    # The sample file ends at 15 s; testdata/README.md lists the book there:
    # bids 150 @ $9.99, asks 100 @ $10.01 and 80 @ $10.015.
    assert app.sidebar.selectbox[0].value == "sample (testdata)"
    assert metrics(app) == {
        "Best bid (150 shares)": "$9.99",
        "Best ask (100 shares)": "$10.01",
        "Spread": "2¢",
        "Mid": "$10.000",
        "Imbalance": "+0.20",
    }


def test_time_buttons_move_through_the_day(app):
    minus = next(b for b in app.sidebar.button if b.label == "−1 s")
    for _ in range(4):  # 15 s -> 11 s
        minus.click().run()
        minus = next(b for b in app.sidebar.button if b.label == "−1 s")
    assert not app.exception
    assert app.sidebar.slider[0].value.second == 11
    assert metrics(app)["Best bid (100 shares)"] == "$10.00"  # at 11 s: 100 @ $10.00 still there
