#pragma once

// Limit order book for a single instrument, with price-time priority.
//
// The book only stores resting orders; it does not match. Matching is done by
// MatchingEngine on top of the primitives here (front / fill_front), and ITCH replay drives
// the book directly, because the exchange has already done the matching.

#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/types.hpp"

namespace lob {

class OrderBook {
public:
    // Adds a resting order at the back of its price level's queue.
    // Returns false (and changes nothing) if the id is already live or quantity is zero.
    bool add(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner = kNoOwner);

    // Removes up to `quantity` shares from a resting order (an execution or a partial
    // cancel); the order leaves the book when it reaches zero. Returns the shares actually
    // removed, which is 0 for an unknown id and less than requested if the order was smaller.
    Quantity reduce(OrderId id, Quantity quantity);

    // Removes a resting order entirely. Returns false for an unknown id.
    bool remove(OrderId id);

    // Cancel-replace: removes `old_id` and adds `new_id` on the same side at the back of the
    // queue at `price`, so the order loses time priority. Returns false and changes nothing
    // if `old_id` is unknown, `new_id` is already live, or `quantity` is zero.
    bool replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity);

    [[nodiscard]] std::optional<OrderInfo> find(OrderId id) const;

    [[nodiscard]] std::optional<LevelInfo> best(Side side) const;
    [[nodiscard]] std::optional<LevelInfo> best_bid() const { return best(Side::kBuy); }
    [[nodiscard]] std::optional<LevelInfo> best_ask() const { return best(Side::kSell); }

    // The oldest order at the best price on `side`: the next one to trade.
    [[nodiscard]] std::optional<OrderInfo> front(Side side) const;

    // Fills `quantity` shares of the front order on `side` and removes it if it is used up.
    // Precondition: the side is non-empty and quantity <= the front order's quantity.
    void fill_front(Side side, Quantity quantity);

    // Calls f(LevelInfo) for up to `max_levels` levels on `side`, best price first.
    template <class F>
    void for_each_level(Side side, std::size_t max_levels, F&& f) const;

    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept { return levels(side).size(); }

private:
    struct Order {
        OrderId id;
        Quantity quantity;
        OwnerId owner;
    };
    struct Level {
        Quantity quantity = 0;
        std::list<Order> orders;  // front is oldest
    };
    // Both sides are sorted ascending; the best bid is the last element, the best ask the first.
    using Levels = std::map<Price, Level>;
    struct Locator {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    Levels& levels(Side side) noexcept { return side == Side::kBuy ? bids_ : asks_; }
    const Levels& levels(Side side) const noexcept { return side == Side::kBuy ? bids_ : asks_; }

    static Levels::iterator best_level(Levels& levels, Side side) {
        return side == Side::kBuy ? std::prev(levels.end()) : levels.begin();
    }
    static Levels::const_iterator best_level(const Levels& levels, Side side) {
        return side == Side::kBuy ? std::prev(levels.end()) : levels.begin();
    }

    void erase(std::unordered_map<OrderId, Locator>::iterator entry);

    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Locator> index_;
};

template <class F>
void OrderBook::for_each_level(Side side, std::size_t max_levels, F&& f) const {
    const Levels& book = levels(side);
    std::size_t n = 0;
    const auto emit = [&](const auto& entry) {
        f(LevelInfo{entry.first, entry.second.quantity,
                    static_cast<std::uint32_t>(entry.second.orders.size())});
        return ++n < max_levels;
    };
    if (max_levels == 0) return;
    if (side == Side::kBuy) {
        for (auto it = book.rbegin(); it != book.rend(); ++it)
            if (!emit(*it)) return;
    } else {
        for (auto it = book.begin(); it != book.end(); ++it)
            if (!emit(*it)) return;
    }
}

}  // namespace lob
