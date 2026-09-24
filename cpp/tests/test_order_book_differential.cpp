// Differential test: lob::OrderBook against the frozen v1 implementation in
// reference_order_book.hpp. Both receive the same random stream of operations and must agree
// on every return value and on the complete book state after every operation.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_order_book.hpp"

using lob::LevelInfo;
using lob::Side;

namespace {

template <class Book>
std::vector<LevelInfo> all_levels(const Book& book, Side side) {
    std::vector<LevelInfo> out;
    book.for_each_level(side, static_cast<std::size_t>(-1),
                        [&](const LevelInfo& l) { out.push_back(l); });
    return out;
}

void expect_same_state(const lob::OrderBook& book, const lob::reference::OrderBook& ref) {
    ASSERT_EQ(book.order_count(), ref.order_count());
    ASSERT_EQ(book.empty(), ref.empty());
    for (const Side side : {Side::kBuy, Side::kSell}) {
        ASSERT_EQ(book.level_count(side), ref.level_count(side));
        ASSERT_EQ(book.best(side), ref.best(side));
        ASSERT_EQ(book.front(side), ref.front(side));
        ASSERT_EQ(all_levels(book, side), all_levels(ref, side));
    }
}

struct Params {
    std::uint32_t seed;
    std::uint32_t price_levels;  // width of the price band each side draws from
    int steps;
};

void run(const Params& p) {
    std::mt19937 rng(p.seed);
    const auto uniform = [&](std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
    };

    lob::OrderBook book;
    lob::reference::OrderBook ref;
    lob::OrderId next_id = 1;
    const auto any_id = [&] {
        return next_id == 1 ? lob::OrderId{1}
                            : lob::OrderId{uniform(1, static_cast<std::uint32_t>(next_id))};
    };

    for (int step = 0; step < p.steps; ++step) {
        SCOPED_TRACE(::testing::Message() << "seed " << p.seed << " step " << step);
        const std::uint32_t op = uniform(0, 99);
        const Side side = uniform(0, 1) == 0 ? Side::kBuy : Side::kSell;
        // Bids below 10,000, asks above, plus some overlap so far-from-touch and best-level
        // changes both happen. The book itself never matches, so overlap is allowed.
        const lob::Price price = side == Side::kBuy ? 10'000 - uniform(0, p.price_levels)
                                                    : 9'990 + uniform(0, p.price_levels);
        const lob::Quantity qty = uniform(0, 30) == 0 ? 0 : uniform(1, 500);

        if (op < 40) {
            const lob::OrderId id = uniform(0, 19) == 0 ? any_id() : next_id++;
            const auto owner = uniform(0, 3);
            ASSERT_EQ(book.add(id, side, price, qty, owner), ref.add(id, side, price, qty, owner));
        } else if (op < 55) {
            const lob::OrderId id = any_id();
            ASSERT_EQ(book.reduce(id, qty), ref.reduce(id, qty));
        } else if (op < 75) {
            const lob::OrderId id = any_id();
            ASSERT_EQ(book.remove(id), ref.remove(id));
        } else if (op < 90) {
            const lob::OrderId old_id = any_id();
            const lob::OrderId new_id = uniform(0, 9) == 0 ? any_id() : next_id++;
            ASSERT_EQ(book.replace(old_id, new_id, price, qty),
                      ref.replace(old_id, new_id, price, qty));
        } else {
            const auto front = ref.front(side);
            if (front) {
                const lob::Quantity fill = uniform(1, front->quantity);
                book.fill_front(side, fill);
                ref.fill_front(side, fill);
            }
        }
        expect_same_state(book, ref);
        if (::testing::Test::HasFatalFailure()) return;
    }

    for (lob::OrderId id = 1; id <= next_id; ++id) {
        ASSERT_EQ(book.find(id), ref.find(id)) << "order " << id;
    }
}

}  // namespace

TEST(OrderBookDifferential, NarrowBookManyOrdersPerLevel) {
    for (std::uint32_t seed = 1; seed <= 4; ++seed) run({seed, 8, 20'000});
}

TEST(OrderBookDifferential, WideBookFewOrdersPerLevel) {
    for (std::uint32_t seed = 11; seed <= 14; ++seed) run({seed, 2'000, 20'000});
}

TEST(OrderBookDifferential, GrowsLargeThenDrains) {
    // Mostly adds first, then mostly removals, so the book grows to thousands of orders and
    // empties again: exercises container growth and reuse of freed storage.
    std::mt19937 rng(99);
    lob::OrderBook book;
    lob::reference::OrderBook ref;
    std::vector<lob::OrderId> live;
    for (lob::OrderId id = 1; id <= 20'000; ++id) {
        const Side side = (rng() & 1) == 0 ? Side::kBuy : Side::kSell;
        const auto price =
            static_cast<lob::Price>(side == Side::kBuy ? 5'000 - rng() % 300 : 5'001 + rng() % 300);
        ASSERT_EQ(book.add(id, side, price, 100), ref.add(id, side, price, 100));
        live.push_back(id);
    }
    expect_same_state(book, ref);
    std::shuffle(live.begin(), live.end(), rng);
    for (std::size_t i = 0; i < live.size(); ++i) {
        ASSERT_EQ(book.remove(live[i]), ref.remove(live[i]));
        if (i % 997 == 0) expect_same_state(book, ref);
    }
    EXPECT_TRUE(book.empty());
    // Reuse after draining.
    ASSERT_TRUE(book.add(1, Side::kBuy, 100, 1));
    ASSERT_TRUE(ref.add(1, Side::kBuy, 100, 1));
    expect_same_state(book, ref);
}
