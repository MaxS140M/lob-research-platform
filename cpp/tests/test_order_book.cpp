#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.hpp"

using lob::LevelInfo;
using lob::OrderBook;
using lob::OrderInfo;
using lob::Side;

namespace {

std::vector<LevelInfo> depth(const OrderBook& book, Side side, std::size_t n = 10) {
    std::vector<LevelInfo> out;
    book.for_each_level(side, n, [&](const LevelInfo& l) { out.push_back(l); });
    return out;
}

}  // namespace

TEST(OrderBook, StartsEmpty) {
    const OrderBook book;
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.front(Side::kBuy));
}

TEST(OrderBook, BestBidIsHighestAndBestAskIsLowest) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.add(2, Side::kBuy, 102, 20);
    book.add(3, Side::kBuy, 101, 30);
    book.add(4, Side::kSell, 105, 40);
    book.add(5, Side::kSell, 103, 50);
    book.add(6, Side::kSell, 104, 60);

    EXPECT_EQ(book.best_bid(), (LevelInfo{102, 20, 1}));
    EXPECT_EQ(book.best_ask(), (LevelInfo{103, 50, 1}));
    EXPECT_EQ(book.order_count(), 6U);
    EXPECT_EQ(book.level_count(Side::kBuy), 3U);
    EXPECT_EQ(book.level_count(Side::kSell), 3U);
}

TEST(OrderBook, LevelsAggregateQuantityAndCount) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.add(2, Side::kBuy, 100, 15);
    book.add(3, Side::kBuy, 99, 5);
    EXPECT_EQ(book.best_bid(), (LevelInfo{100, 25, 2}));
    EXPECT_EQ(depth(book, Side::kBuy), (std::vector<LevelInfo>{{100, 25, 2}, {99, 5, 1}}));
}

TEST(OrderBook, DepthIsBestFirstOnBothSidesAndRespectsLimit) {
    OrderBook book;
    for (lob::Price p = 1; p <= 5; ++p) {
        book.add(p, Side::kBuy, 100 + p, 1);
        book.add(100 + p, Side::kSell, 200 + p, 1);
    }
    const auto bids = depth(book, Side::kBuy, 3);
    const auto asks = depth(book, Side::kSell, 3);
    ASSERT_EQ(bids.size(), 3U);
    ASSERT_EQ(asks.size(), 3U);
    EXPECT_EQ(bids[0].price, 105U);
    EXPECT_EQ(bids[2].price, 103U);
    EXPECT_EQ(asks[0].price, 201U);
    EXPECT_EQ(asks[2].price, 203U);
    EXPECT_TRUE(depth(book, Side::kBuy, 0).empty());
}

TEST(OrderBook, FrontIsOldestOrderAtBestPrice) {
    OrderBook book;
    book.add(1, Side::kSell, 101, 10, 7);
    book.add(2, Side::kSell, 100, 20, 8);
    book.add(3, Side::kSell, 100, 30, 9);
    EXPECT_EQ(book.front(Side::kSell), (OrderInfo{2, Side::kSell, 100, 20, 8}));
}

TEST(OrderBook, RejectsDuplicateIdAndZeroQuantity) {
    OrderBook book;
    EXPECT_TRUE(book.add(1, Side::kBuy, 100, 10));
    EXPECT_FALSE(book.add(1, Side::kSell, 105, 10));
    EXPECT_FALSE(book.add(2, Side::kBuy, 100, 0));
    EXPECT_EQ(book.order_count(), 1U);
    EXPECT_FALSE(book.best_ask());
}

TEST(OrderBook, PartialReduceKeepsPriority) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.add(2, Side::kBuy, 100, 10);
    EXPECT_EQ(book.reduce(1, 4), 4U);
    EXPECT_EQ(book.find(1)->quantity, 6U);
    EXPECT_EQ(book.front(Side::kBuy)->id, 1U);  // still first in the queue
    EXPECT_EQ(book.best_bid(), (LevelInfo{100, 16, 2}));
}

TEST(OrderBook, FullReduceRemovesOrderAndEmptyLevel) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.add(2, Side::kBuy, 99, 10);
    EXPECT_EQ(book.reduce(1, 10), 10U);
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.best_bid()->price, 99U);
    EXPECT_EQ(book.level_count(Side::kBuy), 1U);
}

TEST(OrderBook, OverReduceRemovesOnlyWhatRested) {
    OrderBook book;
    book.add(1, Side::kSell, 100, 10);
    EXPECT_EQ(book.reduce(1, 25), 10U);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBook, UnknownIdsAreReported) {
    OrderBook book;
    EXPECT_EQ(book.reduce(42, 1), 0U);
    EXPECT_FALSE(book.remove(42));
    EXPECT_FALSE(book.replace(42, 43, 100, 1));
    EXPECT_FALSE(book.find(42));
}

TEST(OrderBook, RemoveFromMiddleOfQueue) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 1);
    book.add(2, Side::kBuy, 100, 2);
    book.add(3, Side::kBuy, 100, 3);
    EXPECT_TRUE(book.remove(2));
    EXPECT_EQ(book.best_bid(), (LevelInfo{100, 4, 2}));
    book.fill_front(Side::kBuy, 1);
    EXPECT_EQ(book.front(Side::kBuy)->id, 3U);
}

TEST(OrderBook, ReplaceMovesToBackOfQueueAndKeepsSideAndOwner) {
    OrderBook book;
    book.add(1, Side::kSell, 100, 10, 5);
    book.add(2, Side::kSell, 100, 10);
    ASSERT_TRUE(book.replace(1, 3, 100, 12));
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.find(3), (OrderInfo{3, Side::kSell, 100, 12, 5}));
    EXPECT_EQ(book.front(Side::kSell)->id, 2U);  // 3 lost time priority
    EXPECT_EQ(book.best_ask(), (LevelInfo{100, 22, 2}));
}

TEST(OrderBook, ReplaceToNewPrice) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    ASSERT_TRUE(book.replace(1, 2, 98, 5));
    EXPECT_EQ(book.best_bid(), (LevelInfo{98, 5, 1}));
    EXPECT_EQ(book.level_count(Side::kBuy), 1U);
}

TEST(OrderBook, FailedReplaceChangesNothing) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.add(2, Side::kBuy, 99, 10);
    EXPECT_FALSE(book.replace(1, 2, 101, 10));  // new id already live
    EXPECT_FALSE(book.replace(1, 3, 101, 0));   // zero quantity
    EXPECT_EQ(book.find(1), (OrderInfo{1, Side::kBuy, 100, 10, lob::kNoOwner}));
    EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBook, FillFrontPartialAndFull) {
    OrderBook book;
    book.add(1, Side::kSell, 100, 10);
    book.add(2, Side::kSell, 101, 10);
    book.fill_front(Side::kSell, 4);
    EXPECT_EQ(book.front(Side::kSell)->quantity, 6U);
    book.fill_front(Side::kSell, 6);
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.best_ask(), (LevelInfo{101, 10, 1}));
}

TEST(OrderBook, IdCanBeReusedAfterRemoval) {
    OrderBook book;
    book.add(1, Side::kBuy, 100, 10);
    book.remove(1);
    EXPECT_TRUE(book.add(1, Side::kSell, 105, 3));
    EXPECT_EQ(book.find(1)->side, Side::kSell);
}
