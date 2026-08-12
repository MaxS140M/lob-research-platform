#include <gtest/gtest.h>

#include "lob/itch_parser.hpp"
#include "lob/order_book.hpp"

TEST(OrderBook, StartsEmpty) {
    const lob::OrderBook book;
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0U);
}

TEST(ItchParser, StartsWithNoMessagesParsed) {
    const lob::ItchParser parser;
    EXPECT_EQ(parser.messages_parsed(), 0U);
}
