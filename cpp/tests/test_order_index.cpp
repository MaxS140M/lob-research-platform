#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/order_index.hpp"

using lob::OrderIndex;

TEST(OrderIndex, StartsEmpty) {
    const OrderIndex index;
    EXPECT_TRUE(index.empty());
    EXPECT_EQ(index.find(1), OrderIndex::kMissing);
    EXPECT_FALSE(index.contains(0));
}

TEST(OrderIndex, InsertFindErase) {
    OrderIndex index;
    std::uint32_t* slot = index.try_insert(42);
    ASSERT_NE(slot, nullptr);
    *slot = 7;
    EXPECT_EQ(index.find(42), 7U);
    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.try_insert(42), nullptr);  // duplicate
    EXPECT_TRUE(index.erase(42));
    EXPECT_FALSE(index.erase(42));
    EXPECT_EQ(index.find(42), OrderIndex::kMissing);
    EXPECT_TRUE(index.empty());
}

TEST(OrderIndex, ZeroAndMaxKeysAreOrdinaryKeys) {
    OrderIndex index;
    *index.try_insert(0) = 1;
    *index.try_insert(UINT64_MAX) = 2;
    EXPECT_EQ(index.find(0), 1U);
    EXPECT_EQ(index.find(UINT64_MAX), 2U);
}

TEST(OrderIndex, GrowsAndKeepsLoadAtMostHalf) {
    OrderIndex index;
    for (std::uint32_t k = 1; k <= 10'000; ++k) {
        *index.try_insert(k) = k;
        ASSERT_LE(index.size() * 2, index.capacity());
    }
    for (std::uint32_t k = 1; k <= 10'000; ++k) ASSERT_EQ(index.find(k), k);
}

TEST(OrderIndex, ReserveAvoidsRehash) {
    OrderIndex index(1'000);
    const std::size_t capacity = index.capacity();
    for (std::uint32_t k = 0; k < 1'000; ++k) *index.try_insert(k) = k;
    EXPECT_EQ(index.capacity(), capacity);
}

// Random inserts and erases against std::unordered_map, with a small key space so probe runs
// collide, wrap around the end of the table, and get backward-shifted constantly.
TEST(OrderIndex, MatchesUnorderedMapUnderChurn) {
    for (const std::uint64_t key_space : {64ULL, 1'000ULL, 1ULL << 40}) {
        std::mt19937_64 rng(key_space);
        OrderIndex index;
        std::unordered_map<std::uint64_t, std::uint32_t> model;
        for (std::uint32_t step = 0; step < 200'000; ++step) {
            // Spread keys out by a large odd stride, like real order references can be.
            const std::uint64_t key = (rng() % key_space) * 0x10001ULL;
            if (rng() % 3 != 0) {
                std::uint32_t* slot = index.try_insert(key);
                const bool inserted = model.try_emplace(key, step).second;
                ASSERT_EQ(slot != nullptr, inserted);
                if (slot) *slot = step;
            } else {
                ASSERT_EQ(index.erase(key), model.erase(key) == 1);
            }
            ASSERT_EQ(index.size(), model.size());
            if (step % 1'000 == 0) {
                for (const auto& [k, v] : model) ASSERT_EQ(index.find(k), v);
            }
        }
        for (std::uint64_t k = 0; k < std::min<std::uint64_t>(key_space, 5'000); ++k) {
            const std::uint64_t key = k * 0x10001ULL;
            const auto it = model.find(key);
            ASSERT_EQ(index.find(key), it == model.end() ? OrderIndex::kMissing : it->second);
        }
    }
}
