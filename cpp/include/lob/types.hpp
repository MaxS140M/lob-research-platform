#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::uint32_t;  // 4 implied decimals, as on the ITCH wire: 1823500 == $182.35
using Quantity = std::uint32_t;
using OwnerId = std::uint32_t;  // trading account, used for self-trade prevention

// Orders with no owner (e.g. replayed from market data) never count as self-trades.
inline constexpr OwnerId kNoOwner = 0;

enum class Side : std::uint8_t { kBuy, kSell };

constexpr Side opposite(Side s) noexcept { return s == Side::kBuy ? Side::kSell : Side::kBuy; }

// Aggregate state of one price level.
struct LevelInfo {
    Price price;
    Quantity quantity;
    std::uint32_t order_count;

    friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

// State of one resting order.
struct OrderInfo {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    OwnerId owner;

    friend bool operator==(const OrderInfo&, const OrderInfo&) = default;
};

}  // namespace lob
