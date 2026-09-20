#include "lob/order_book.hpp"

namespace lob {

bool OrderBook::add(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner) {
    if (quantity == 0) return false;
    const auto [entry, inserted] = index_.try_emplace(id);
    if (!inserted) return false;

    Level& level = levels(side)[price];
    level.quantity += quantity;
    level.orders.push_back(Order{id, quantity, owner});
    entry->second = Locator{side, price, std::prev(level.orders.end())};
    return true;
}

Quantity OrderBook::reduce(OrderId id, Quantity quantity) {
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

bool OrderBook::remove(OrderId id) {
    const auto entry = index_.find(id);
    if (entry == index_.end()) return false;
    erase(entry);
    return true;
}

bool OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
    if (quantity == 0) return false;
    const auto entry = index_.find(old_id);
    if (entry == index_.end()) return false;
    if (new_id != old_id && index_.contains(new_id)) return false;

    const Side side = entry->second.side;
    const OwnerId owner = entry->second.it->owner;
    erase(entry);
    return add(new_id, side, price, quantity, owner);
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const {
    const auto entry = index_.find(id);
    if (entry == index_.end()) return std::nullopt;
    const Locator& loc = entry->second;
    return OrderInfo{id, loc.side, loc.price, loc.it->quantity, loc.it->owner};
}

std::optional<LevelInfo> OrderBook::best(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const auto it = best_level(book, side);
    return LevelInfo{it->first, it->second.quantity,
                     static_cast<std::uint32_t>(it->second.orders.size())};
}

std::optional<OrderInfo> OrderBook::front(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const auto it = best_level(book, side);
    const Order& order = it->second.orders.front();
    return OrderInfo{order.id, side, it->first, order.quantity, order.owner};
}

void OrderBook::fill_front(Side side, Quantity quantity) {
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

void OrderBook::erase(std::unordered_map<OrderId, Locator>::iterator entry) {
    const Locator& loc = entry->second;
    Levels& book = levels(loc.side);
    const auto level = book.find(loc.price);
    level->second.quantity -= loc.it->quantity;
    level->second.orders.erase(loc.it);
    if (level->second.orders.empty()) book.erase(level);
    index_.erase(entry);
}

}  // namespace lob
