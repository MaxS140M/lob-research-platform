#include <gtest/gtest.h>

#include "lob/book_builder.hpp"

using lob::BookBuilder;
using lob::LevelInfo;
using lob::Side;
namespace itch = lob::itch;

namespace {

constexpr std::uint16_t kAapl = 7;
constexpr std::uint16_t kMsft = 9;

itch::Symbol sym(std::string_view s) {
    itch::Symbol out;
    out.fill(' ');
    for (std::size_t i = 0; i < s.size() && i < out.size(); ++i) out[i] = s[i];
    return out;
}

itch::StockDirectory directory(std::uint16_t locate, std::string_view s) {
    itch::StockDirectory m{};
    m.stock_locate = locate;
    m.stock = sym(s);
    return m;
}

itch::AddOrder add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
                   std::uint32_t price) {
    itch::AddOrder m{};
    m.stock_locate = locate;
    m.order_ref = ref;
    m.side = side;
    m.shares = shares;
    m.price = price;
    return m;
}

itch::OrderExecuted executed(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    itch::OrderExecuted m{};
    m.stock_locate = locate;
    m.order_ref = ref;
    m.executed_shares = shares;
    return m;
}

itch::OrderCancel cancel(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    itch::OrderCancel m{};
    m.stock_locate = locate;
    m.order_ref = ref;
    m.cancelled_shares = shares;
    return m;
}

itch::OrderDelete del(std::uint16_t locate, std::uint64_t ref) {
    itch::OrderDelete m{};
    m.stock_locate = locate;
    m.order_ref = ref;
    return m;
}

itch::OrderReplace replace(std::uint16_t locate, std::uint64_t old_ref, std::uint64_t new_ref,
                           std::uint32_t shares, std::uint32_t price) {
    itch::OrderReplace m{};
    m.stock_locate = locate;
    m.original_order_ref = old_ref;
    m.new_order_ref = new_ref;
    m.shares = shares;
    m.price = price;
    return m;
}

}  // namespace

TEST(BookBuilder, RoutesMessagesToTheBookForEachLocate) {
    BookBuilder b;
    b.on_stock_directory(directory(kAapl, "AAPL"));
    b.on_stock_directory(directory(kMsft, "MSFT"));
    b.on_add_order(add(kAapl, 1, 'B', 100, 1'823'500));
    b.on_add_order(add(kMsft, 2, 'S', 50, 1'401'000));

    EXPECT_EQ(b.book(kAapl).best_bid(), (LevelInfo{1'823'500, 100, 1}));
    EXPECT_FALSE(b.book(kAapl).best_ask());
    EXPECT_EQ(b.book(kMsft).best_ask(), (LevelInfo{1'401'000, 50, 1}));
    EXPECT_EQ(b.locate("AAPL"), kAapl);
    EXPECT_EQ(b.symbol(kMsft), "MSFT");
    EXPECT_FALSE(b.locate("TSLA"));
    EXPECT_EQ(b.total_orders(), 2U);
}

TEST(BookBuilder, AppliesExecuteCancelDeleteAndReplace) {
    BookBuilder b;
    b.on_add_order(add(kAapl, 1, 'S', 100, 1'000'000));
    b.on_add_order(add(kAapl, 2, 'S', 100, 1'000'100));
    b.on_add_order(add(kAapl, 3, 'B', 100, 999'900));

    b.on_order_executed(executed(kAapl, 1, 30));  // 70 left at the best ask
    EXPECT_EQ(b.book(kAapl).best_ask(), (LevelInfo{1'000'000, 70, 1}));

    b.on_order_cancel(cancel(kAapl, 1, 70));  // cancelling the rest removes it
    EXPECT_EQ(b.book(kAapl).best_ask(), (LevelInfo{1'000'100, 100, 1}));

    b.on_order_replace(replace(kAapl, 3, 4, 200, 999'800));
    EXPECT_FALSE(b.book(kAapl).find(3));
    EXPECT_EQ(b.book(kAapl).find(4)->side, Side::kBuy);
    EXPECT_EQ(b.book(kAapl).best_bid(), (LevelInfo{999'800, 200, 1}));

    b.on_order_delete(del(kAapl, 2));
    EXPECT_FALSE(b.book(kAapl).best_ask());
    EXPECT_EQ(b.anomalies().total(), 0U);
}

TEST(BookBuilder, ExecutedWithPriceReducesTheOrder) {
    BookBuilder b;
    b.on_add_order(add(kAapl, 1, 'B', 100, 1'000'000));
    itch::OrderExecutedWithPrice m{};
    m.stock_locate = kAapl;
    m.order_ref = 1;
    m.executed_shares = 40;
    m.execution_price = 999'900;
    b.on_order_executed_with_price(m);
    EXPECT_EQ(b.book(kAapl).find(1)->quantity, 60U);
}

TEST(BookBuilder, CountsAnomaliesInsteadOfFailing) {
    BookBuilder b;
    b.on_add_order(add(kAapl, 1, 'B', 100, 1'000'000));
    b.on_add_order(add(kAapl, 1, 'B', 100, 1'000'000));  // duplicate
    b.on_order_delete(del(kAapl, 99));                   // unknown
    b.on_order_executed(executed(kMsft, 1, 10));         // wrong locate: unknown there
    b.on_order_cancel(cancel(kAapl, 1, 150));            // more than rests
    b.on_add_order(add(kAapl, 5, 'S', 10, 1'000'100));
    b.on_add_order(add(kAapl, 6, 'S', 10, 1'000'200));
    b.on_order_replace(replace(kAapl, 5, 6, 10, 1'000'300));  // new ref already live

    const auto& a = b.anomalies();
    EXPECT_EQ(a.duplicate_adds, 1U);
    EXPECT_EQ(a.unknown_orders, 2U);
    EXPECT_EQ(a.over_reductions, 1U);
    EXPECT_EQ(a.failed_replaces, 1U);
    EXPECT_TRUE(b.book(kAapl).find(5));  // failed replace left the original in place
}

TEST(BookBuilder, UnseenLocateHasAnEmptyBook) {
    const BookBuilder b;
    EXPECT_TRUE(b.book(12345).empty());
}
