#include "lob/order_book.hpp"

#include <algorithm>
#include <type_traits>

namespace lob {

// Inserting or erasing a level moves the levels after it. std::list keeps its nodes (and so
// the iterators held in the index) valid across a move, as long as the move cannot throw and
// fall back to a copy.
static_assert(std::is_nothrow_move_constructible_v<std::list<int>>);
static_assert(std::is_nothrow_move_assignable_v<std::list<int>>);

std::size_t OrderBook::lower_level(const Levels& levels, Side side, Price price) noexcept {
    // Linear scan back from the touch, where nearly all activity is; fall back to binary
    // search if the price turns out to be deep in the book.
    constexpr std::size_t kLinearSteps = 16;
    std::size_t i = levels.size();
    for (std::size_t step = 0; step < kLinearSteps; ++step) {
        if (i == 0 || worse(side, levels[i - 1].price, price)) return i;
        --i;
    }
    const auto it = std::lower_bound(
        levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(i), price,
        [side](const Level& level, Price p) { return worse(side, level.price, p); });
    return static_cast<std::size_t>(it - levels.begin());
}

bool OrderBook::add(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner) {
    if (quantity == 0) return false;
    const auto [entry, inserted] = index_.try_emplace(id);
    if (!inserted) return false;

    Levels& book = levels(side);
    const std::size_t i = lower_level(book, side, price);
    if (i == book.size() || book[i].price != price) {
        book.insert(book.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 0, {}});
    }
    Level& level = book[i];
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
    Levels& book = levels(entry->second.side);
    book[lower_level(book, entry->second.side, entry->second.price)].quantity -= quantity;
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
    const Level& level = book.back();
    return LevelInfo{level.price, level.quantity, static_cast<std::uint32_t>(level.orders.size())};
}

std::optional<OrderInfo> OrderBook::front(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const Level& level = book.back();
    const Order& order = level.orders.front();
    return OrderInfo{order.id, side, level.price, order.quantity, order.owner};
}

void OrderBook::fill_front(Side side, Quantity quantity) {
    Level& level = levels(side).back();
    Order& order = level.orders.front();
    if (quantity >= order.quantity) {
        erase(index_.find(order.id));
        return;
    }
    order.quantity -= quantity;
    level.quantity -= quantity;
}

void OrderBook::erase(std::unordered_map<OrderId, Locator>::iterator entry) {
    const Locator& loc = entry->second;
    Levels& book = levels(loc.side);
    const std::size_t i = lower_level(book, loc.side, loc.price);
    Level& level = book[i];
    level.quantity -= loc.it->quantity;
    level.orders.erase(loc.it);
    if (level.orders.empty()) book.erase(book.begin() + static_cast<std::ptrdiff_t>(i));
    index_.erase(entry);
}

}  // namespace lob
