#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "lob/matching_engine.hpp"

using lob::Fill;
using lob::LevelInfo;
using lob::MatchingEngine;
using lob::NewOrder;
using lob::OrderInfo;
using lob::OrderType;
using lob::SelfTradePrevention;
using lob::Side;
using lob::SubmitResult;
using lob::SubmitStatus;

namespace {

NewOrder limit(lob::OrderId id, Side side, lob::Price price, lob::Quantity qty,
               lob::OwnerId owner = lob::kNoOwner) {
    return {id, side, OrderType::kLimit, price, qty, owner};
}
NewOrder market(lob::OrderId id, Side side, lob::Quantity qty) {
    return {id, side, OrderType::kMarket, 0, qty, lob::kNoOwner};
}
NewOrder ioc(lob::OrderId id, Side side, lob::Price price, lob::Quantity qty) {
    return {id, side, OrderType::kImmediateOrCancel, price, qty, lob::kNoOwner};
}

// Submits and records every fill.
struct Harness {
    MatchingEngine engine;
    std::vector<Fill> fills;

    explicit Harness(SelfTradePrevention stp = SelfTradePrevention::kCancelIncoming)
        : engine(stp) {}

    SubmitResult submit(const NewOrder& o) {
        return engine.submit(o, [&](const Fill& f) { fills.push_back(f); });
    }
    const lob::OrderBook& book() const { return engine.book(); }
};

}  // namespace

// ---- Resting and simple matches -------------------------------------------

TEST(MatchingEngine, NonCrossingLimitOrdersRest) {
    Harness h;
    const auto r1 = h.submit(limit(1, Side::kBuy, 100, 10));
    const auto r2 = h.submit(limit(2, Side::kSell, 101, 10));
    EXPECT_TRUE(r1.accepted());
    EXPECT_EQ(r1.rested, 10U);
    EXPECT_EQ(r2.rested, 10U);
    EXPECT_TRUE(h.fills.empty());
    EXPECT_EQ(h.book().best_bid()->price, 100U);
    EXPECT_EQ(h.book().best_ask()->price, 101U);
}

TEST(MatchingEngine, ExactSizeMatchEmptiesBothSides) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 10));
    const auto r = h.submit(limit(2, Side::kBuy, 100, 10));

    EXPECT_EQ(r.filled, 10U);
    EXPECT_EQ(r.rested, 0U);
    EXPECT_EQ(r.cancelled, 0U);
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.fills[0], (Fill{2, 1, Side::kBuy, 100, 10}));
    EXPECT_TRUE(h.book().empty());
}

TEST(MatchingEngine, TradesAtRestingPriceNotAggressorLimit) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 10));
    h.submit(limit(2, Side::kBuy, 105, 10));
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.fills[0].price, 100U);  // price improvement goes to the aggressor
}

TEST(MatchingEngine, PartialFillRestsRemainderAtLimit) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 4));
    const auto r = h.submit(limit(2, Side::kBuy, 100, 10));
    EXPECT_EQ(r.filled, 4U);
    EXPECT_EQ(r.rested, 6U);
    EXPECT_FALSE(h.book().best_ask());
    EXPECT_EQ(h.book().best_bid(), (LevelInfo{100, 6, 1}));
}

TEST(MatchingEngine, IncomingSmallerThanRestingLeavesRestingPartlyFilled) {
    Harness h;
    h.submit(limit(1, Side::kBuy, 100, 10));
    h.submit(limit(2, Side::kSell, 100, 3));
    EXPECT_EQ(h.book().find(1)->quantity, 7U);
    EXPECT_FALSE(h.book().find(2));
}

// ---- Priority --------------------------------------------------------------

TEST(MatchingEngine, OldestOrderAtAPriceFillsFirst) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    h.submit(limit(2, Side::kSell, 100, 5));
    h.submit(limit(3, Side::kSell, 100, 5));
    h.submit(limit(4, Side::kBuy, 100, 7));

    ASSERT_EQ(h.fills.size(), 2U);
    EXPECT_EQ(h.fills[0], (Fill{4, 1, Side::kBuy, 100, 5}));
    EXPECT_EQ(h.fills[1], (Fill{4, 2, Side::kBuy, 100, 2}));
    EXPECT_EQ(h.book().front(Side::kSell)->id, 2U);  // partly filled, keeps its place
}

TEST(MatchingEngine, SweepsSeveralLevelsBestPriceFirst) {
    Harness h;
    h.submit(limit(1, Side::kSell, 102, 5));
    h.submit(limit(2, Side::kSell, 100, 5));
    h.submit(limit(3, Side::kSell, 101, 5));
    h.submit(limit(4, Side::kSell, 103, 5));

    const auto r = h.submit(limit(5, Side::kBuy, 102, 20));

    EXPECT_EQ(r.filled, 15U);
    EXPECT_EQ(r.rested, 5U);
    EXPECT_EQ(r.fill_count, 3U);
    ASSERT_EQ(h.fills.size(), 3U);
    EXPECT_EQ(h.fills[0], (Fill{5, 2, Side::kBuy, 100, 5}));
    EXPECT_EQ(h.fills[1], (Fill{5, 3, Side::kBuy, 101, 5}));
    EXPECT_EQ(h.fills[2], (Fill{5, 1, Side::kBuy, 102, 5}));
    // Stopped at its limit: the 103 offer is untouched and the rest bids at 102.
    EXPECT_EQ(h.book().best_ask(), (LevelInfo{103, 5, 1}));
    EXPECT_EQ(h.book().best_bid(), (LevelInfo{102, 5, 1}));
}

TEST(MatchingEngine, SellSideSweepIsSymmetric) {
    Harness h;
    h.submit(limit(1, Side::kBuy, 98, 5));
    h.submit(limit(2, Side::kBuy, 100, 5));
    h.submit(limit(3, Side::kBuy, 99, 5));
    h.submit(limit(4, Side::kSell, 99, 12));

    ASSERT_EQ(h.fills.size(), 2U);
    EXPECT_EQ(h.fills[0], (Fill{4, 2, Side::kSell, 100, 5}));
    EXPECT_EQ(h.fills[1], (Fill{4, 3, Side::kSell, 99, 5}));
    EXPECT_EQ(h.book().best_ask(), (LevelInfo{99, 2, 1}));
    EXPECT_EQ(h.book().best_bid(), (LevelInfo{98, 5, 1}));
}

// ---- Market and IOC --------------------------------------------------------

TEST(MatchingEngine, MarketOrderSweepsAnyPriceAndNeverRests) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    h.submit(limit(2, Side::kSell, 250, 5));
    const auto r = h.submit(market(3, Side::kBuy, 12));

    EXPECT_EQ(r.filled, 10U);
    EXPECT_EQ(r.cancelled, 2U);
    EXPECT_EQ(r.rested, 0U);
    EXPECT_EQ(h.fills.back().price, 250U);
    EXPECT_TRUE(h.book().empty());
}

TEST(MatchingEngine, MarketOrderIntoEmptyBookIsCancelled) {
    Harness h;
    const auto r = h.submit(market(1, Side::kSell, 10));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.cancelled, 10U);
    EXPECT_TRUE(h.book().empty());
}

TEST(MatchingEngine, IocFillsWhatCrossesAndCancelsTheRest) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    h.submit(limit(2, Side::kSell, 102, 5));
    const auto r = h.submit(ioc(3, Side::kBuy, 101, 10));

    EXPECT_EQ(r.filled, 5U);
    EXPECT_EQ(r.cancelled, 5U);
    EXPECT_EQ(r.rested, 0U);
    EXPECT_FALSE(h.book().best_bid());  // never rests
    EXPECT_EQ(h.book().best_ask(), (LevelInfo{102, 5, 1}));
}

TEST(MatchingEngine, IocThatDoesNotCrossIsFullyCancelled) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    const auto r = h.submit(ioc(2, Side::kBuy, 99, 5));
    EXPECT_EQ(r.filled, 0U);
    EXPECT_EQ(r.cancelled, 5U);
    EXPECT_EQ(h.book().order_count(), 1U);
}

// ---- Cancel ----------------------------------------------------------------

TEST(MatchingEngine, CancelPartlyFilledOrderRemovesTheRemainder) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 10));
    h.submit(limit(2, Side::kBuy, 100, 4));
    ASSERT_EQ(h.book().find(1)->quantity, 6U);

    EXPECT_TRUE(h.engine.cancel(1));
    EXPECT_TRUE(h.book().empty());
    EXPECT_FALSE(h.engine.cancel(1));  // already gone
}

TEST(MatchingEngine, CancelFullyFilledOrderFails) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 10));
    h.submit(limit(2, Side::kBuy, 100, 10));
    EXPECT_FALSE(h.engine.cancel(1));
    EXPECT_FALSE(h.engine.cancel(2));
}

TEST(MatchingEngine, CancelledOrderNoLongerTrades) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 10));
    h.submit(limit(2, Side::kSell, 101, 10));
    h.engine.cancel(1);
    h.submit(limit(3, Side::kBuy, 101, 5));
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.fills[0].resting_id, 2U);
}

// ---- Self-trade prevention -------------------------------------------------

TEST(MatchingEngine, SelfTradeCancelIncomingLeavesRestingOrder) {
    Harness h(SelfTradePrevention::kCancelIncoming);
    h.submit(limit(1, Side::kSell, 100, 10, /*owner=*/7));
    const auto r = h.submit(limit(2, Side::kBuy, 100, 10, /*owner=*/7));

    EXPECT_TRUE(h.fills.empty());
    EXPECT_EQ(r.cancelled, 10U);
    EXPECT_EQ(r.rested, 0U);  // a limit order stopped by STP does not rest
    EXPECT_EQ(h.book().find(1)->quantity, 10U);
    EXPECT_FALSE(h.book().best_bid());
}

TEST(MatchingEngine, SelfTradeCancelIncomingKeepsEarlierFills) {
    Harness h(SelfTradePrevention::kCancelIncoming);
    h.submit(limit(1, Side::kSell, 100, 5, 8));  // someone else, first in queue
    h.submit(limit(2, Side::kSell, 100, 5, 7));  // own order behind it
    const auto r = h.submit(limit(3, Side::kBuy, 100, 10, 7));

    EXPECT_EQ(r.filled, 5U);
    EXPECT_EQ(r.cancelled, 5U);
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.fills[0].resting_id, 1U);
    EXPECT_TRUE(h.book().find(2));
}

TEST(MatchingEngine, SelfTradeCancelRestingRemovesOwnOrderAndContinues) {
    Harness h(SelfTradePrevention::kCancelResting);
    h.submit(limit(1, Side::kSell, 100, 5, 7));  // own order at the front
    h.submit(limit(2, Side::kSell, 100, 5, 8));
    const auto r = h.submit(limit(3, Side::kBuy, 100, 5, 7));

    EXPECT_EQ(r.self_trade_cancels, 1U);
    EXPECT_EQ(r.filled, 5U);
    ASSERT_EQ(h.fills.size(), 1U);
    EXPECT_EQ(h.fills[0].resting_id, 2U);
    EXPECT_TRUE(h.book().empty());
}

TEST(MatchingEngine, AnonymousOrdersNeverSelfTrade) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    h.submit(limit(2, Side::kBuy, 100, 5));
    EXPECT_EQ(h.fills.size(), 1U);
}

// ---- Rejections ------------------------------------------------------------

TEST(MatchingEngine, RejectsInvalidOrdersWithoutTouchingTheBook) {
    Harness h;
    h.submit(limit(1, Side::kSell, 100, 5));
    EXPECT_EQ(h.submit(limit(1, Side::kBuy, 100, 5)).status, SubmitStatus::kRejectedDuplicateId);
    EXPECT_EQ(h.submit(limit(2, Side::kBuy, 100, 0)).status, SubmitStatus::kRejectedZeroQuantity);
    EXPECT_EQ(h.submit(limit(3, Side::kBuy, 0, 5)).status, SubmitStatus::kRejectedZeroPrice);
    EXPECT_EQ(h.submit(ioc(4, Side::kBuy, 0, 5)).status, SubmitStatus::kRejectedZeroPrice);
    EXPECT_TRUE(h.fills.empty());
    EXPECT_EQ(h.book().find(1)->quantity, 5U);
}

// ---- Differential test against a brute-force model -------------------------

namespace {

// Obviously-correct reference: a flat list of resting orders, searched in full for the best
// price (then lowest sequence number) on every match step.
class NaiveEngine {
public:
    explicit NaiveEngine(SelfTradePrevention stp) : stp_(stp) {}

    SubmitResult submit(const NewOrder& o, std::vector<Fill>& fills) {
        SubmitResult r;
        if (o.quantity == 0) return reject(SubmitStatus::kRejectedZeroQuantity);
        if (o.type != OrderType::kMarket && o.price == 0)
            return reject(SubmitStatus::kRejectedZeroPrice);
        if (find(o.id) != resting_.end()) return reject(SubmitStatus::kRejectedDuplicateId);

        lob::Quantity remaining = o.quantity;
        bool stp_stop = false;
        while (remaining > 0) {
            auto best = resting_.end();
            for (auto it = resting_.begin(); it != resting_.end(); ++it) {
                if (it->side == o.side) continue;
                if (best == resting_.end() || better(it->side, *it, *best)) best = it;
            }
            if (best == resting_.end()) break;
            if (o.type != OrderType::kMarket) {
                const bool crosses =
                    o.side == Side::kBuy ? o.price >= best->price : o.price <= best->price;
                if (!crosses) break;
            }
            if (o.owner != lob::kNoOwner && best->owner == o.owner) {
                if (stp_ == SelfTradePrevention::kCancelResting) {
                    resting_.erase(best);
                    ++r.self_trade_cancels;
                    continue;
                }
                stp_stop = true;
                break;
            }
            const lob::Quantity q = std::min(remaining, best->quantity);
            fills.push_back(Fill{o.id, best->id, o.side, best->price, q});
            remaining -= q;
            r.filled += q;
            ++r.fill_count;
            best->quantity -= q;
            if (best->quantity == 0) resting_.erase(best);
        }
        if (remaining > 0) {
            if (o.type == OrderType::kLimit && !stp_stop) {
                resting_.push_back({o.id, o.side, o.price, remaining, o.owner, seq_++});
                r.rested = remaining;
            } else {
                r.cancelled = remaining;
            }
        }
        return r;
    }

    bool cancel(lob::OrderId id) {
        const auto it = find(id);
        if (it == resting_.end()) return false;
        resting_.erase(it);
        return true;
    }

    std::optional<OrderInfo> lookup(lob::OrderId id) const {
        for (const auto& o : resting_)
            if (o.id == id) return OrderInfo{o.id, o.side, o.price, o.quantity, o.owner};
        return std::nullopt;
    }

    std::optional<lob::Price> best_price(Side side) const {
        std::optional<lob::Price> p;
        for (const auto& o : resting_) {
            if (o.side != side) continue;
            if (!p || (side == Side::kBuy ? o.price > *p : o.price < *p)) p = o.price;
        }
        return p;
    }

    std::size_t size() const { return resting_.size(); }

private:
    struct Resting {
        lob::OrderId id;
        Side side;
        lob::Price price;
        lob::Quantity quantity;
        lob::OwnerId owner;
        std::uint64_t seq;
    };

    static SubmitResult reject(SubmitStatus s) {
        SubmitResult r;
        r.status = s;
        return r;
    }
    static bool better(Side side, const Resting& a, const Resting& b) {
        if (a.price != b.price) return side == Side::kBuy ? a.price > b.price : a.price < b.price;
        return a.seq < b.seq;
    }
    std::vector<Resting>::iterator find(lob::OrderId id) {
        return std::find_if(resting_.begin(), resting_.end(),
                            [&](const Resting& r) { return r.id == id; });
    }

    SelfTradePrevention stp_;
    std::vector<Resting> resting_;
    std::uint64_t seq_ = 0;
};

void expect_same_result(const SubmitResult& a, const SubmitResult& b) {
    EXPECT_EQ(a.status, b.status);
    EXPECT_EQ(a.filled, b.filled);
    EXPECT_EQ(a.rested, b.rested);
    EXPECT_EQ(a.cancelled, b.cancelled);
    EXPECT_EQ(a.fill_count, b.fill_count);
    EXPECT_EQ(a.self_trade_cancels, b.self_trade_cancels);
}

void run_differential(SelfTradePrevention stp, std::uint32_t seed, int steps) {
    std::mt19937 rng(seed);
    const auto uniform = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };

    MatchingEngine engine(stp);
    NaiveEngine naive(stp);
    lob::OrderId next_id = 1;
    std::uint64_t total_fills = 0;
    std::uint64_t self_trade_events = 0;

    for (int step = 0; step < steps; ++step) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed << " step " << step);
        const std::uint32_t action = uniform(0, 99);

        if (action < 25 && next_id > 1) {
            // Cancel a random earlier id: may be resting, partly filled, filled or cancelled.
            const lob::OrderId id = uniform(1, static_cast<std::uint32_t>(next_id - 1));
            ASSERT_EQ(engine.cancel(id), naive.cancel(id));
        } else {
            NewOrder o{};
            // Occasionally reuse a live id to exercise the duplicate check.
            o.id = (action == 99 && next_id > 1) ? next_id - 1 : next_id++;
            o.side = uniform(0, 1) == 0 ? Side::kBuy : Side::kSell;
            const std::uint32_t t = uniform(0, 9);
            o.type = t < 7 ? OrderType::kLimit
                           : (t < 9 ? OrderType::kImmediateOrCancel : OrderType::kMarket);
            o.price = uniform(95, 105);  // narrow band so orders cross often
            o.quantity = uniform(0, 20) == 0 ? 0 : uniform(1, 30);
            o.owner = uniform(0, 3);  // 0 = anonymous

            std::vector<Fill> engine_fills;
            std::vector<Fill> naive_fills;
            const auto a = engine.submit(o, [&](const Fill& f) { engine_fills.push_back(f); });
            const auto b = naive.submit(o, naive_fills);
            expect_same_result(a, b);
            total_fills += a.fill_count;
            self_trade_events += a.self_trade_cancels;
            if (a.accepted() && a.cancelled > 0 && o.type == OrderType::kLimit) ++self_trade_events;
            ASSERT_EQ(engine_fills, naive_fills);
            if (a.accepted()) {
                ASSERT_EQ(a.filled + a.rested + a.cancelled, o.quantity);  // every share accounted
            }
        }

        // Book state must agree, and the book must never be left crossed.
        ASSERT_EQ(engine.book().order_count(), naive.size());
        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        ASSERT_EQ(bid ? std::optional<lob::Price>(bid->price) : std::nullopt,
                  naive.best_price(Side::kBuy));
        ASSERT_EQ(ask ? std::optional<lob::Price>(ask->price) : std::nullopt,
                  naive.best_price(Side::kSell));
        if (bid && ask) {
            ASSERT_LT(bid->price, ask->price);
        }
    }

    for (lob::OrderId id = 1; id < next_id; ++id) {
        ASSERT_EQ(engine.book().find(id), naive.lookup(id)) << "order " << id;
    }
    // Guard against a vacuous pass: the random stream must actually trade and self-trade.
    EXPECT_GT(total_fills, static_cast<std::uint64_t>(steps) / 4);
    EXPECT_GT(self_trade_events, 0U);
}

}  // namespace

TEST(MatchingEngineDifferential, MatchesNaiveModelCancelIncoming) {
    for (std::uint32_t seed = 1; seed <= 5; ++seed) {
        run_differential(SelfTradePrevention::kCancelIncoming, seed, 10'000);
    }
}

TEST(MatchingEngineDifferential, MatchesNaiveModelCancelResting) {
    for (std::uint32_t seed = 101; seed <= 105; ++seed) {
        run_differential(SelfTradePrevention::kCancelResting, seed, 10'000);
    }
}
