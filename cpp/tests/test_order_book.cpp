#include <gtest/gtest.h>

#include "lob/order_book.hpp"

TEST(OrderBook, StartsEmpty) {
    const lob::OrderBook book;
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0U);
}
