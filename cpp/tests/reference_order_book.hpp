#pragma once

// Reference order book: the original v1 implementation (std::map of levels, std::list per
// level, std::unordered_map index), frozen as an obviously-correct model. Optimised
// versions of lob::OrderBook are tested against it operation by operation.
//
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

namespace lob::reference {


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


// ---- Implementation ----


inline bool OrderBook::add(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner) {
    if (quantity == 0) return false;
    const auto [entry, inserted] = index_.try_emplace(id);
    if (!inserted) return false;

    Level& level = levels(side)[price];
    level.quantity += quantity;
    level.orders.push_back(Order{id, quantity, owner});
    entry->second = Locator{side, price, std::prev(level.orders.end())};
    return true;
}

inline Quantity OrderBook::reduce(OrderId id, Quantity quantity) {
    const auto entry = index_.find(id);
    if (entry == index_.end()) return 0;

    Order& order = *entry->second.it;
    if (quantity >= order.quantity) {
        const Quantity removed = order.quantity;
        erase(entry);
        return removed;
    }
    order.quantity -= quantity;
    levels(entry->second.side).find(entry->second.price)->second.quantity -= quantity;
    return quantity;
}

inline bool OrderBook::remove(OrderId id) {
    const auto entry = index_.find(id);
    if (entry == index_.end()) return false;
    erase(entry);
    return true;
}

inline bool OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
    if (quantity == 0) return false;
    const auto entry = index_.find(old_id);
    if (entry == index_.end()) return false;
    if (new_id != old_id && index_.contains(new_id)) return false;

    const Side side = entry->second.side;
    const OwnerId owner = entry->second.it->owner;
    erase(entry);
    return add(new_id, side, price, quantity, owner);
}

inline std::optional<OrderInfo> OrderBook::find(OrderId id) const {
    const auto entry = index_.find(id);
    if (entry == index_.end()) return std::nullopt;
    const Locator& loc = entry->second;
    return OrderInfo{id, loc.side, loc.price, loc.it->quantity, loc.it->owner};
}

inline std::optional<LevelInfo> OrderBook::best(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const auto it = best_level(book, side);
    return LevelInfo{it->first, it->second.quantity,
                     static_cast<std::uint32_t>(it->second.orders.size())};
}

inline std::optional<OrderInfo> OrderBook::front(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const auto it = best_level(book, side);
    const Order& order = it->second.orders.front();
    return OrderInfo{order.id, side, it->first, order.quantity, order.owner};
}

inline void OrderBook::fill_front(Side side, Quantity quantity) {
    Levels& book = levels(side);
    const auto level = best_level(book, side);
    Order& order = level->second.orders.front();
    if (quantity >= order.quantity) {
        erase(index_.find(order.id));
        return;
    }
    order.quantity -= quantity;
    level->second.quantity -= quantity;
}

inline void OrderBook::erase(std::unordered_map<OrderId, Locator>::iterator entry) {
    const Locator& loc = entry->second;
    Levels& book = levels(loc.side);
    const auto level = book.find(loc.price);
    level->second.quantity -= loc.it->quantity;
    level->second.orders.erase(loc.it);
    if (level->second.orders.empty()) book.erase(level);
    index_.erase(entry);
}
}  // namespace lob::reference
