"""Order book explorer: scrub through a trading day and watch the book change.

    streamlit run dashboard/app.py

Pick a day and a stock, move the time slider, and see the top 10 price levels on each side,
recent trades, the price around that moment, and the 5-second model's prediction.

Reads ITCH files from LOB_DATA_DIR (default: data/). The model panel needs the five BX days
used in the research (see docs/getting-started.md); without them the rest still works, and
testdata/sample.itch can be explored on its own.
"""

from __future__ import annotations

import os
from datetime import time, timedelta
from pathlib import Path

import altair as alt
import numpy as np
import pandas as pd
import streamlit as st

import lobpy

SEC = 1_000_000_000
MINUTE, HOUR = 60 * SEC, 3600 * SEC
OPEN, CLOSE = 9 * HOUR + 30 * MINUTE, 16 * HOUR
DEPTH = 10

# The ML setup from docs/research-report.md: trained on these days, never on the holdout.
DEV_DAYS = ["20190730", "20190830", "20191030", "20191230"]
HOLDOUT_DAY = "20200130"
RESEARCH_SYMBOLS = ["SPY", "QQQ", "IWM", "AAPL", "MSFT"]
GRID_NS = 250_000_000

# Validated chart palette (same as the docs): bids blue, asks orange; down red, up blue.
BLUE, ORANGE, RED, GRAY = "#2a78d6", "#eb6834", "#e34948", "#8a8984"

DATA_DIR = Path(os.environ.get("LOB_DATA_DIR", "data"))
CACHE_DIR = Path(os.environ.get("LOB_CACHE_DIR", Path.home() / "lob-cache"))


# ---- Data (cached: each replay of a full day takes a fraction of a second) -------------------


def available_files() -> dict[str, Path]:
    """Every ITCH file the app can open, by display name."""
    files = {p.name.split(".")[0]: p for p in sorted(DATA_DIR.glob("*ITCH_50"))}
    sample = Path(__file__).resolve().parents[1] / "testdata" / "sample.itch"
    if sample.exists():
        files["sample (testdata)"] = sample
    return files


@st.cache_resource(show_spinner=False)
def directory(path: str) -> lobpy.Replayer:
    return lobpy.Replayer(path)


@st.cache_resource(show_spinner=False)
def replayer(path: str, symbol: str) -> lobpy.Replayer:
    return lobpy.Replayer(path, [symbol])


@st.cache_data(show_spinner=False)
def book_at(path: str, symbol: str, t: int) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Top DEPTH levels on each side, as of time t (every message with timestamp <= t)."""
    s = replayer(path, symbol).snapshots(SEC, depth=DEPTH, start_ns=t, end_ns=t)
    sides = []
    for side in ("bid", "ask"):
        price, size = s[f"{side}_price"][0], s[f"{side}_size"][0]
        keep = size > 0
        sides.append(pd.DataFrame({"price": price[keep] / 1e4, "shares": size[keep]}))
    return sides[0], sides[1]


@st.cache_data(show_spinner=False)
def day_prices(path: str, symbol: str, start: int, end: int) -> pd.DataFrame:
    """Best bid and ask every second over [start, end]."""
    s = replayer(path, symbol).snapshots(SEC, depth=1, start_ns=start, end_ns=end)
    frame = pd.DataFrame({"t": s["timestamp"]})
    for side in ("bid", "ask"):
        frame[side] = np.where(s[f"{side}_size"][:, 0] > 0, s[f"{side}_price"][:, 0] / 1e4, np.nan)
    return frame


@st.cache_data(show_spinner=False)
def day_trades(path: str, symbol: str) -> pd.DataFrame:
    t = replayer(path, symbol).trades()
    frame = pd.DataFrame(
        {
            "t": t["timestamp"],
            "price": t["price"] / 1e4,
            "shares": t["shares"],
            "side": t["side"],
        }
    )
    return frame[frame["side"] != 0].reset_index(drop=True)


@st.cache_resource(show_spinner="Training the 5-second model on the four development days...")
def model() -> object | None:
    """LightGBM for the 5 s direction label, trained on the development days only."""
    from lobpy.research import FEATURES
    from lobpy.research.models import GradientBoosting

    paths = [CACHE_DIR / f"research_{d}.pkl" for d in DEV_DAYS]
    if not all(p.exists() for p in paths):
        return None
    dev = pd.concat([pd.read_pickle(p) for p in paths], ignore_index=True)
    dev = dev[dev["label_5s"].notna()]
    return GradientBoosting().fit(dev[FEATURES], dev["label_5s"].to_numpy())


@st.cache_data(show_spinner=False)
def research_features(day: str) -> pd.DataFrame | None:
    path = CACHE_DIR / f"research_{day}.pkl"
    return pd.read_pickle(path) if path.exists() else None


# ---- Formatting helpers --------------------------------------------------------------------------


def clock(ns: int) -> str:
    s = ns // SEC
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def to_ns(t: time) -> int:
    return ((t.hour * 60 + t.minute) * 60 + t.second) * SEC


def to_time(ns: int) -> time:
    s = ns // SEC
    return time(s // 3600, s % 3600 // 60, s % 60)


# ---- Page ---------------------------------------------------------------------------------------

st.set_page_config(page_title="Order book explorer", page_icon="📈", layout="wide")
st.title("Order book explorer")
st.caption(
    "Real NASDAQ BX market data, rebuilt message by message by the C++ engine. "
    "Pick a moment and see the book exactly as it stood."
)

files = available_files()
if not files:
    st.error(f"No ITCH files found in `{DATA_DIR}`. See docs/getting-started.md to download data.")
    st.stop()


def parse_clock(text: str | None) -> int | None:
    """ "10:00:08" -> ns since midnight, or None if missing or malformed."""
    try:
        h, m, s = (int(x) for x in (text or "").split(":"))
        return ((h * 60 + m) * 60 + s) * SEC
    except ValueError:
        return None


# Links can open a specific moment, e.g. ?day=20200130&symbol=SPY&t=10:00:11
query = st.query_params

with st.sidebar:
    st.header("Choose a moment")
    names = list(files)
    # Open on the holdout day if it is there: the model never saw it in training.
    start_day = query.get("day", HOLDOUT_DAY)
    day = st.selectbox("Day", names, index=names.index(start_day) if start_day in names else 0)
    path = str(files[day])
    with st.spinner("Reading the day's stock list..."):
        symbols = directory(path).symbols
    wanted = query.get("symbol")
    default = (
        wanted
        if wanted in symbols
        else next((s for s in ("SPY", "AAPL") if s in symbols), symbols[0])
    )
    symbol = st.selectbox("Stock", symbols, index=symbols.index(default))

    r = directory(path)
    lo = max(r.first_timestamp, OPEN) if r.last_timestamp > OPEN else r.first_timestamp
    hi = min(r.last_timestamp, CLOSE) if r.last_timestamp > OPEN else r.last_timestamp
    # The chosen time lives in the slider's session state; the buttons nudge it.
    if "slider" not in st.session_state or not lo <= to_ns(st.session_state.slider) <= hi:
        start_t = parse_clock(query.get("t")) or 10 * HOUR
        st.session_state.slider = to_time(min(max(start_t, lo), hi))

    def nudge(delta: int) -> None:
        st.session_state.slider = to_time(min(max(to_ns(st.session_state.slider) + delta, lo), hi))

    picked = st.slider(
        "Time (ET)",
        min_value=to_time(lo),
        max_value=to_time(hi),
        step=timedelta(seconds=1),
        format="HH:mm:ss",
        key="slider",
    )
    row1, row2 = st.columns(2), st.columns(2)
    row1[0].button("−1 min", on_click=nudge, args=(-MINUTE,), width="stretch")
    row1[1].button("+1 min", on_click=nudge, args=(MINUTE,), width="stretch")
    row2[0].button("−1 s", on_click=nudge, args=(-SEC,), width="stretch")
    row2[1].button("+1 s", on_click=nudge, args=(SEC,), width="stretch")

t = to_ns(picked)
with st.spinner("Replaying the day up to this moment..."):
    bids, asks = book_at(path, symbol, t)

# ---- Headline numbers ----
best_bid = bids["price"].iloc[0] if len(bids) else None
best_ask = asks["price"].iloc[0] if len(asks) else None
m = st.columns(5)
m[0].metric(
    f"Best bid ({bids['shares'].iloc[0]:,} shares)" if best_bid else "Best bid",
    f"${best_bid:,.2f}" if best_bid else "–",
)
m[1].metric(
    f"Best ask ({asks['shares'].iloc[0]:,} shares)" if best_ask else "Best ask",
    f"${best_ask:,.2f}" if best_ask else "–",
)
spread_cents = round((best_ask - best_bid) * 100) if best_bid and best_ask else None
m[2].metric("Spread", f"{spread_cents}¢" if spread_cents is not None else "–")
m[3].metric("Mid", f"${(best_bid + best_ask) / 2:,.3f}" if spread_cents is not None else "–")
if best_bid and best_ask:
    b, a = bids["shares"].iloc[0], asks["shares"].iloc[0]
    m[4].metric(
        "Imbalance",
        f"{(b - a) / (b + a):+.2f}",
        help="(bid size − ask size) / total "
        "at the best prices. Positive means more buyers waiting.",
    )
else:
    m[4].metric("Imbalance", "–")

left, right = st.columns([3, 2], gap="large")

# ---- The book ladder ----
with left:
    st.subheader(f"{symbol} at {clock(t)}: top {DEPTH} levels")
    ladder = pd.concat([bids.assign(side="Bid"), asks.assign(side="Ask")], ignore_index=True)
    if ladder.empty:
        st.info("The book is empty at this moment.")
    else:
        ladder["label"] = ladder["price"].map(lambda p: f"${p:,.2f}")
        chart = (
            alt.Chart(ladder)
            .mark_bar(cornerRadiusEnd=3, height=14)
            .encode(
                y=alt.Y("label:N", sort=alt.SortField("price", order="descending"), title=None),
                x=alt.X("shares:Q", title="Shares waiting"),
                color=alt.Color(
                    "side:N",
                    scale=alt.Scale(domain=["Bid", "Ask"], range=[BLUE, ORANGE]),
                    legend=alt.Legend(orient="top", title=None),
                ),
                tooltip=[
                    alt.Tooltip("side:N"),
                    alt.Tooltip("price:Q", format="$,.2f"),
                    alt.Tooltip("shares:Q", format=","),
                ],
            )
            .properties(height=max(200, 22 * len(ladder)))
        )
        st.altair_chart(chart, width="stretch")
        st.caption("Asks (sellers) on top, bids (buyers) below. The gap between is the spread.")

# ---- The model's prediction ----
with right:
    st.subheader("Model: next 5 seconds")
    feats = research_features(day) if day in [*DEV_DAYS, HOLDOUT_DAY] else None
    clf = model() if feats is not None and symbol in RESEARCH_SYMBOLS else None
    if clf is None:
        st.info(
            "The model covers SPY, QQQ, IWM, AAPL and MSFT on the five research days, and needs "
            "the feature cache from `scripts/run_research.py`."
        )
    else:
        from lobpy.research import FEATURES

        rows = feats[(feats["symbol"] == symbol) & (feats["timestamp"] <= t)]
        if rows.empty:
            st.info("No prediction before 09:35 (the study starts then).")
        else:
            row = rows.iloc[[-1]]
            p = clf.predict_proba(row[FEATURES])[0]
            probs = pd.DataFrame({"move": ["Down", "Flat", "Up"], "probability": p})
            bars = (
                alt.Chart(probs)
                .mark_bar(cornerRadiusEnd=3)
                .encode(
                    x=alt.X("move:N", sort=None, title=None),
                    y=alt.Y(
                        "probability:Q",
                        scale=alt.Scale(domain=[0, 1]),
                        axis=alt.Axis(format="%"),
                        title="Probability",
                    ),
                    color=alt.Color(
                        "move:N",
                        scale=alt.Scale(domain=["Down", "Flat", "Up"], range=[RED, GRAY, BLUE]),
                        legend=None,
                    ),
                    tooltip=[alt.Tooltip("move:N"), alt.Tooltip("probability:Q", format=".1%")],
                )
                .properties(height=220)
            )
            labels = bars.mark_text(dy=-8).encode(text=alt.Text("probability:Q", format=".0%"))
            st.altair_chart(bars + labels, width="stretch")

            if day == HOLDOUT_DAY:
                st.caption("30 Jan 2020 is the **holdout day**: the model never saw it.")
            else:
                st.warning("This day was used to train the model, so this prediction is in-sample.")
            confident = max(p[0], p[2]) > 0.6
            if spread_cents is not None and spread_cents > 2 and confident:
                st.error(
                    f"Confident, but the spread is {spread_cents}¢. This is the research finding: "
                    "the model is most sure exactly when trading costs the most."
                )
            elif confident:
                st.success("Confident, and the spread is tight enough to trade.")

# ---- Price and trades around this moment ----
st.subheader("Price and trades, 5 minutes either side")
start, end = max(t - 5 * MINUTE, lo), min(t + 5 * MINUTE, hi)
prices = day_prices(path, symbol, start, end)
trades = day_trades(path, symbol)
window = trades[(trades["t"] >= start) & (trades["t"] <= end)].copy()
prices["time"] = pd.to_datetime(prices["t"], unit="ns")
window["time"] = pd.to_datetime(window["t"], unit="ns")
window["aggressor"] = np.where(window["side"] > 0, "Buyer-initiated", "Seller-initiated")
long = prices.melt(id_vars="time", value_vars=["bid", "ask"], var_name="side", value_name="price")
long["side"] = long["side"].map({"bid": "Best bid", "ask": "Best ask"})

lines = (
    alt.Chart(long.dropna())
    .mark_line(interpolate="step-after", strokeWidth=1.5)
    .encode(
        x=alt.X("time:T", axis=alt.Axis(format="%H:%M:%S"), title=None),
        y=alt.Y("price:Q", scale=alt.Scale(zero=False), title="Price ($)"),
        color=alt.Color(
            "side:N",
            scale=alt.Scale(domain=["Best bid", "Best ask"], range=[BLUE, ORANGE]),
            legend=alt.Legend(orient="top", title=None),
        ),
        tooltip=[
            alt.Tooltip("time:T", format="%H:%M:%S"),
            "side:N",
            alt.Tooltip("price:Q", format="$,.2f"),
        ],
    )
)
dots = (
    alt.Chart(window)
    .mark_point(filled=True, size=40, color="#52514e")
    .encode(
        x="time:T",
        y="price:Q",
        shape=alt.Shape(
            "aggressor:N",
            scale=alt.Scale(
                domain=["Buyer-initiated", "Seller-initiated"],
                range=["triangle-up", "triangle-down"],
            ),
            legend=alt.Legend(orient="top", title=None),
        ),
        tooltip=[
            alt.Tooltip("time:T", format="%H:%M:%S.%L"),
            "aggressor:N",
            alt.Tooltip("price:Q", format="$,.2f"),
            alt.Tooltip("shares:Q", format=","),
        ],
    )
)
now = (
    alt.Chart(pd.DataFrame({"time": [pd.to_datetime(t, unit="ns")]}))
    .mark_rule(strokeDash=[4, 4], color="#52514e")
    .encode(x="time:T")
)
st.altair_chart((lines + dots + now).properties(height=320), width="stretch")

recent = trades[trades["t"] <= t].tail(10).iloc[::-1]
if len(recent):
    st.subheader("Last 10 trades before this moment")
    st.dataframe(
        pd.DataFrame(
            {
                "time": recent["t"].map(lambda ns: f"{clock(ns)}.{ns % SEC // 1_000_000:03d}"),
                "price": recent["price"].map(lambda p: f"${p:,.2f}"),
                "shares": recent["shares"],
                "aggressor": np.where(recent["side"] > 0, "buyer", "seller"),
            }
        ),
        hide_index=True,
        width="stretch",
    )
