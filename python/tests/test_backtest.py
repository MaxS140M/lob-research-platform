"""Backtester tests. Every expected number here is worked out by hand in the comments."""

from pathlib import Path

import numpy as np
import pytest

import lobpy
from lobpy.backtest import (
    BUY,
    SECOND,
    SELL,
    BacktestConfig,
    Backtester,
    Cancel,
    EventFeed,
    LimitOrder,
    MarketOrder,
    Strategy,
)
from lobpy.backtest.strategies import BuyOpenSellClose, ImbalanceMaker, ImbalanceTaker

SAMPLE = Path(__file__).resolve().parents[2] / "testdata" / "sample.itch"


# ---- Helpers ---------------------------------------------------------------------------------


def make_feed(rows: list[dict], depth: int = 2) -> EventFeed:
    """Builds a feed from hand-written events. Each row gives the event and the book after it."""
    n = len(rows)
    cols = {name: np.zeros(n, dtype=np.int64) for name in (
        "timestamp", "seq", "price", "shares", "trade_price", "priority",
        "old_price", "old_shares", "old_priority")}  # fmt: skip
    cols["kind"] = np.zeros(n, dtype=np.uint8)
    cols["side"] = np.zeros(n, dtype=np.int8)
    for name in ("bid_price", "bid_size", "ask_price", "ask_size"):
        cols[name] = np.zeros((n, depth), dtype=np.int64)
    for i, r in enumerate(rows):
        cols["timestamp"][i] = r["t"]
        cols["seq"][i] = r.get("seq", i + 1)
        cols["kind"][i] = ord(r.get("kind", "A"))
        for name in ("side", "price", "shares", "trade_price", "priority", "old_price",
                     "old_shares", "old_priority"):  # fmt: skip
            cols[name][i] = r.get(name, 0)
        for level, (p, s) in enumerate(r.get("bids", [])):
            cols["bid_price"][i, level], cols["bid_size"][i, level] = p, s
        for level, (p, s) in enumerate(r.get("asks", [])):
            cols["ask_price"][i, level], cols["ask_size"][i, level] = p, s
    return EventFeed(cols, "TEST")


class Scripted:
    """Sends the given actions on the event with the given timestamp, and records everything."""

    def __init__(self, script: dict[int, list]):
        self.script = script
        self.seen: list[int] = []
        self.fills = []

    def on_book_update(self, book):
        self.seen.append(book.timestamp)
        return self.script.get(book.timestamp)

    def on_fill(self, fill):
        self.fills.append(fill)


def config(**overrides) -> BacktestConfig:
    base = dict(latency_ns=1, taker_fee=0.0, maker_fee=0.0, max_position=10_000,
                flatten_at_ns=None, depth=2)  # fmt: skip
    base.update(overrides)
    return BacktestConfig(**base)


# ---- The hand-computed end-to-end case ---------------------------------------------------------


def test_buy_open_sell_close_on_sample_file_matches_hand_calculation():
    # AAA in testdata/sample.itch. The strategy buys 100 at the first event at or after 11 s:
    # the execution at 11.0 s. With 1 ns latency the order is handled against the book after
    # that event: asks 100 @ $10.01, bid $10.00, mid $10.005. So it buys 100 @ $10.01.
    # Forced flat from 13 s: the next AAA event is the cross at 14.0 s. The sell arrives after
    # the last event, against the final book: bid 150 @ $9.99, ask $10.01, mid $10.00.
    #   gross  = (9.99 - 10.01) x 100              = -2.00
    #   spread = (10.005 - 10.01) x 100 + (9.99 - 10.00) x 100 = -0.50 - 1.00 = -1.50
    #   signal = gross - spread = (10.00 - 10.005) x 100      = -0.50
    #   fees   = 200 shares x $0.003                           =  0.60
    #   net    = -2.00 - 0.60                                  = -2.60
    replayer = lobpy.Replayer(str(SAMPLE))
    feed = EventFeed.from_replayer(replayer, "AAA", depth=2)
    cfg = BacktestConfig(latency_ns=1, taker_fee=0.003, maker_fee=0.003,
                         flatten_at_ns=13 * SECOND, depth=2)  # fmt: skip
    result = Backtester(feed, cfg).run(BuyOpenSellClose(quantity=100, open_ns=11 * SECOND))

    fills = result.fills
    assert fills["side"].tolist() == [BUY, SELL]
    assert fills["price"].tolist() == pytest.approx([10.01, 9.99])
    assert fills["quantity"].tolist() == [100, 100]
    assert fills["liquidity"].tolist() == ["taker", "taker"]
    assert fills["time"].tolist() == [np.timedelta64(11 * SECOND + 1, "ns"),
                                      np.timedelta64(14 * SECOND + 1, "ns")]

    m = result.metrics
    assert m["gross_pnl"] == pytest.approx(-2.00)
    assert m["spread_pnl"] == pytest.approx(-1.50)
    assert m["signal_pnl"] == pytest.approx(-0.50)
    assert m["fees"] == pytest.approx(0.60)
    assert m["net_pnl"] == pytest.approx(-2.60)
    assert m["final_position"] == 0
    assert m["fill_rate"] == pytest.approx(1.0)
    assert m["turnover"] == pytest.approx(200 / cfg.max_position)
    assert m["net_pnl"] == pytest.approx(m["signal_pnl"] + m["spread_pnl"] - m["fees"])


# ---- Latency ---------------------------------------------------------------------------------

LATENCY_ROWS = [
    {"t": 1_000, "bids": [(100, 500)], "asks": [(101, 500)]},
    {"t": 1_050, "kind": "D", "side": SELL, "price": 101, "shares": 500,
     "bids": [(100, 500)], "asks": [(102, 500)]},  # the $1.01 offer disappears at t = 1050
    {"t": 2_000, "bids": [(100, 500)], "asks": [(102, 500)]},
]  # fmt: skip


@pytest.mark.parametrize("latency, expected_price", [(10, 101), (100, 102)])
def test_latency_decides_which_book_the_order_meets(latency, expected_price):
    # Sent at t = 1000. With 10 ns it arrives at 1010, before the offer is pulled; with 100 ns
    # it arrives at 1100, after, and has to pay the next level.
    strategy = Scripted({1_000: [MarketOrder(BUY, 100)]})
    Backtester(make_feed(LATENCY_ROWS), config(latency_ns=latency)).run(strategy)
    assert [(f.price, f.quantity, f.timestamp) for f in strategy.fills] == [
        (expected_price, 100, 1_000 + latency)
    ]


def test_strategy_sees_each_event_once_in_order():
    strategy = Scripted({})
    Backtester(make_feed(LATENCY_ROWS), config()).run(strategy)
    assert strategy.seen == [1_000, 1_050, 2_000]
    assert isinstance(strategy, Strategy)


# ---- Crossing the spread -----------------------------------------------------------------------


def test_market_order_walks_the_book_and_cancels_what_it_cannot_fill():
    rows = [
        {"t": 10, "bids": [(99, 10)], "asks": [(101, 50), (102, 30)]},
        {"t": 20, "bids": [(99, 10)], "asks": [(101, 50), (102, 30)]},
    ]
    strategy = Scripted({10: [MarketOrder(BUY, 100)]})
    result = Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.price, f.quantity, f.liquidity) for f in strategy.fills] == [
        (101, 50, "taker"), (102, 30, "taker")]  # fmt: skip
    assert result.metrics["fill_rate"] == pytest.approx(0.8)  # 20 of 100 shares cancelled


def test_sell_market_order_receives_the_bid():
    rows = [{"t": 10, "bids": [(99, 500)], "asks": [(101, 500)]}, {"t": 20}]
    strategy = Scripted({10: [MarketOrder(SELL, 100)]})
    Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.price, f.quantity) for f in strategy.fills] == [(99, 100)]


def test_marketable_limit_takes_up_to_its_price_then_rests():
    rows = [
        {"t": 10, "bids": [(99, 10)], "asks": [(101, 50), (102, 30)]},
        {"t": 20, "bids": [(99, 10)], "asks": [(102, 30)]},
    ]
    strategy = Scripted({10: [LimitOrder(BUY, 80, 101)]})
    result = Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.price, f.quantity) for f in strategy.fills] == [(101, 50)]
    assert "1 orders still resting" in " ".join(result.notes)


# ---- Queue position ----------------------------------------------------------------------------


def queue_rows():
    """Bid queue at 100: order #1 (60 shares, priority 1) then #2 (40, priority 2)."""
    return [
        {"t": 10, "kind": "A", "side": BUY, "price": 100, "shares": 60, "priority": 1,
         "bids": [(100, 60)], "asks": [(105, 100)]},
        # Our buy 50 @ 100 is sent here and joins behind both: ahead = 100.
        {"t": 20, "kind": "A", "side": BUY, "price": 100, "shares": 40, "priority": 2,
         "bids": [(100, 100)], "asks": [(105, 100)]},
    ]  # fmt: skip


def test_queue_position_fifo_fills():
    rows = queue_rows() + [
        # #1 cancels 30 (ahead 100 -> 70), then executes 30 (-> 40).
        {"t": 30, "kind": "X", "side": BUY, "price": 100, "shares": 30, "priority": 1,
         "bids": [(100, 70)], "asks": [(105, 100)]},
        {"t": 40, "kind": "E", "side": BUY, "price": 100, "shares": 30, "priority": 1,
         "trade_price": 100, "bids": [(100, 40)], "asks": [(105, 100)]},
        # A new order #5 joins behind us; it does not change what is ahead of us.
        {"t": 50, "kind": "A", "side": BUY, "price": 100, "shares": 100, "priority": 5,
         "bids": [(100, 140)], "asks": [(105, 100)]},
        # #2 executes 40: nothing is ahead of us now (40 -> 0), but we are not filled yet.
        {"t": 60, "kind": "E", "side": BUY, "price": 100, "shares": 40, "priority": 2,
         "trade_price": 100, "bids": [(100, 100)], "asks": [(105, 100)]},
        # #5, behind us, executes 20: in FIFO those 20 shares would have hit us first.
        {"t": 70, "kind": "E", "side": BUY, "price": 100, "shares": 20, "priority": 5,
         "trade_price": 100, "bids": [(100, 80)], "asks": [(105, 100)]},
        # A bid at 99 executes: the market traded through 100, so the rest of ours fills.
        {"t": 80, "kind": "E", "side": BUY, "price": 99, "shares": 10, "priority": 7,
         "trade_price": 99, "bids": [(100, 80)], "asks": [(105, 100)]},
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)]})
    Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.timestamp, f.price, f.quantity, f.liquidity) for f in strategy.fills] == [
        (70, 100, 20, "maker"),
        (80, 100, 30, "maker"),
    ]


def test_cancels_behind_us_do_not_move_us_up():
    rows = queue_rows() + [
        {"t": 30, "kind": "A", "side": BUY, "price": 100, "shares": 50, "priority": 3,
         "bids": [(100, 150)], "asks": [(105, 100)]},
        {"t": 40, "kind": "X", "side": BUY, "price": 100, "shares": 50, "priority": 3,
         "bids": [(100, 100)], "asks": [(105, 100)]},  # behind us: ahead stays 100
        {"t": 50, "kind": "E", "side": BUY, "price": 100, "shares": 60, "priority": 1,
         "trade_price": 100, "bids": [(100, 40)], "asks": [(105, 100)]},  # ahead 100 -> 40
        {"t": 60, "kind": "D", "side": BUY, "price": 100, "shares": 40, "priority": 2,
         "bids": [], "asks": [(105, 100)]},  # ahead 40 -> 0, still no trade
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)]})
    Backtester(make_feed(rows), config()).run(strategy)
    assert strategy.fills == []  # nobody ever sold at 100


def test_replace_ahead_of_us_moves_us_up():
    rows = queue_rows() + [
        # #1 is replaced to a new price: its 60 shares leave our queue (ahead 100 -> 40).
        {"t": 30, "kind": "U", "side": BUY, "price": 98, "shares": 60, "priority": 3,
         "old_price": 100, "old_shares": 60, "old_priority": 1,
         "bids": [(100, 40), (98, 60)], "asks": [(105, 100)]},
        # #2 executes 50 against an aggressive seller: 40 were ahead, so 10 reach us.
        {"t": 40, "kind": "E", "side": BUY, "price": 100, "shares": 40, "priority": 2,
         "trade_price": 100, "bids": [(98, 60)], "asks": [(105, 100)]},
        {"t": 50, "kind": "P", "side": BUY, "price": 100, "shares": 10, "trade_price": 100,
         "bids": [(98, 60)], "asks": [(105, 100)]},  # hidden buy at 100 trades; we're first
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)]})
    Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.timestamp, f.quantity) for f in strategy.fills] == [(50, 10)]


def test_opposite_side_reaching_our_price_fills_us():
    rows = queue_rows() + [
        {"t": 30, "kind": "A", "side": SELL, "price": 100, "shares": 500,
         "bids": [(100, 100)], "asks": [(100, 500)]},  # an offer at our bid price
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)]})
    Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.timestamp, f.price, f.quantity, f.liquidity) for f in strategy.fills] == [
        (30, 100, 50, "maker")]


def test_cancel_request_takes_effect_after_latency():
    rows = queue_rows() + [
        {"t": 30, "kind": "A", "side": BUY, "price": 100, "shares": 10, "priority": 3,
         "bids": [(100, 110)], "asks": [(105, 100)]},
        # Our cancel is sent at 30 but only arrives at 30 + 100 = 130, after this trade-through.
        {"t": 60, "kind": "E", "side": BUY, "price": 99, "shares": 5, "trade_price": 99,
         "priority": 4, "bids": [(100, 110)], "asks": [(105, 100)]},
        {"t": 200, "bids": [(100, 110)], "asks": [(105, 100)]},
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)], 30: [Cancel(1)]})
    Backtester(make_feed(rows), config(latency_ns=1)).run(strategy)
    assert [f.quantity for f in strategy.fills] == []  # cancelled at 31, before the trade

    strategy = Scripted({20: [LimitOrder(BUY, 50, 100)], 30: [Cancel(1)]})
    Backtester(make_feed(rows), config(latency_ns=35)).run(strategy)  # cancel lands at 65
    assert [(f.timestamp, f.quantity) for f in strategy.fills] == [(60, 50)]


# ---- Fees, risk and the forced flat --------------------------------------------------------------


def test_fees_are_charged_per_share_by_liquidity_type():
    rows = queue_rows() + [
        {"t": 30, "kind": "A", "side": SELL, "price": 100, "shares": 500,
         "bids": [(100, 100)], "asks": [(100, 500)]},
    ]  # fmt: skip
    strategy = Scripted({20: [LimitOrder(BUY, 50, 100), MarketOrder(BUY, 10)]})
    cfg = config(taker_fee=0.0030, maker_fee=-0.0020)  # a maker rebate
    result = Backtester(make_feed(rows), cfg).run(strategy)
    fees = {f.liquidity: f.fee for f in strategy.fills}
    assert fees == {"taker": 30 * 10, "maker": -20 * 50}  # price units: $0.003 = 30
    assert result.metrics["fees"] == pytest.approx((300 - 1000) / 10_000)


def test_risk_limit_counts_open_orders():
    rows = [{"t": 10, "bids": [(99, 1000)], "asks": [(101, 1000)]}, {"t": 20}]
    strategy = Scripted({10: [MarketOrder(BUY, 100), MarketOrder(BUY, 1), MarketOrder(SELL, 250),
                              MarketOrder(SELL, 200), LimitOrder(BUY, 10, 0)]})  # fmt: skip
    result = Backtester(make_feed(rows), config(max_position=100)).run(strategy)
    # Limit 100, checked as if every open order fills (or, for the other side, never fills):
    #   BUY 100: worst long  = 0 + 0 + 100 = 100  -> ok
    #   BUY 1:   worst long  = 0 + 100 + 1 = 101  -> rejected
    #   SELL 250: worst short = 0 + 0 + 250 = 250 -> rejected
    #   SELL 200: worst short = 200, if the in-flight buy never fills -> rejected
    #   LimitOrder at price 0 -> invalid, rejected
    assert result.metrics["orders"] == 5
    assert result.metrics["rejected_orders"] == 4
    assert result.metrics["max_abs_position"] == 100


def test_forced_flat_cancels_orders_and_closes_the_position():
    rows = [
        {"t": 10, "bids": [(99, 1000)], "asks": [(101, 1000)]},
        {"t": 20, "bids": [(99, 1000)], "asks": [(101, 1000)]},
        {"t": 30, "bids": [(99, 1000)], "asks": [(101, 1000)]},  # flatten from here
        {"t": 40, "bids": [(99, 1000)], "asks": [(101, 1000)]},
        {"t": 50, "bids": [(99, 1000)], "asks": [(101, 1000)]},
    ]
    strategy = Scripted({10: [MarketOrder(BUY, 100), LimitOrder(BUY, 50, 95)],
                         40: [MarketOrder(BUY, 100)]})  # fmt: skip  # ignored after 30
    result = Backtester(make_feed(rows), config(flatten_at_ns=30)).run(strategy)
    assert result.metrics["final_position"] == 0
    assert strategy.seen == [10, 20]  # the strategy is not consulted once flattening starts
    assert [(f.side, f.price, f.quantity) for f in strategy.fills] == [(BUY, 101, 100),
                                                                       (SELL, 99, 100)]
    assert result.metrics["fill_rate"] == pytest.approx(200 / 250)  # the resting 50 was cancelled


# ---- Metrics ---------------------------------------------------------------------------------


def test_drawdown_and_breakdown_identity_on_example_strategies():
    replayer = lobpy.Replayer(str(SAMPLE))
    feed = EventFeed.from_replayer(replayer, "AAA", depth=2)
    for strategy in (ImbalanceTaker(threshold=0.1, hold_ns=SECOND // 2),
                     ImbalanceMaker(threshold=0.1, hold_ns=SECOND // 2)):  # fmt: skip
        m = Backtester(feed, BacktestConfig(depth=2, flatten_at_ns=13 * SECOND)).run(strategy).metrics
        assert m["net_pnl"] == pytest.approx(m["signal_pnl"] + m["spread_pnl"] - m["fees"])
        assert m["max_drawdown"] >= 0
        assert m["final_position"] == 0


def test_depth_beyond_feed_is_rejected():
    with pytest.raises(ValueError):
        Backtester(make_feed(LATENCY_ROWS, depth=2), config(depth=5))


def test_ioc_limit_cancels_instead_of_resting():
    rows = [
        {"t": 10, "bids": [(99, 10)], "asks": [(101, 50), (102, 30)]},
        {"t": 20, "kind": "A", "side": BUY, "price": 99, "shares": 1, "priority": 9,
         "bids": [(99, 11)], "asks": [(101, 50), (102, 30)]},
    ]  # fmt: skip
    strategy = Scripted({10: [LimitOrder(BUY, 80, 101, ioc=True)]})
    result = Backtester(make_feed(rows), config()).run(strategy)
    assert [(f.price, f.quantity) for f in strategy.fills] == [(101, 50)]
    assert not any("resting" in note for note in result.notes)  # the 30 were cancelled
    assert result.metrics["fill_rate"] == pytest.approx(50 / 80)


def test_forced_flat_never_walks_a_thin_book():
    # Long 100 when flattening starts, but only 30 shares are bid at the touch (99) and the
    # next bid is far away (90). The backtester sells at the touch only, event after event.
    rows = [
        {"t": 10, "bids": [(99, 1000)], "asks": [(101, 1000)]},
        {"t": 20, "bids": [(99, 30), (90, 1000)], "asks": [(101, 1000)]},
        {"t": 30, "bids": [(99, 50), (90, 1000)], "asks": [(101, 1000)]},
        {"t": 40, "bids": [(98, 100), (90, 1000)], "asks": [(101, 1000)]},
    ]
    strategy = Scripted({10: [MarketOrder(BUY, 100)]})
    result = Backtester(make_feed(rows), config(flatten_at_ns=20)).run(strategy)
    assert [(f.side, f.price, f.quantity) for f in strategy.fills] == [
        (BUY, 101, 100), (SELL, 99, 30), (SELL, 99, 50), (SELL, 98, 20)]  # fmt: skip
    assert result.metrics["final_position"] == 0
