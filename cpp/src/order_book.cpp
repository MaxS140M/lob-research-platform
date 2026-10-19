// Order book implementation. The interesting parts:
//   lower_level()  finds a price level, searching back from the best price
//   add() / erase() link orders into and out of their level's queue (a list threaded through
//                  the order pool by index), creating or removing levels as needed

#include "lob/order_book.hpp"

#include <algorithm>
#include <type_traits>

namespace lob {

OrderBook::OrderBook(std::size_t expected_orders) {
    pool_.reserve(expected_orders);
    index_.reserve(expected_orders);
}

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

OrderBook::NodeIndex OrderBook::allocate() {
    if (free_head_ != kNull) {
        const NodeIndex n = free_head_;
        free_head_ = pool_[n].next;
        return n;
    }
    pool_.emplace_back();
    return static_cast<NodeIndex>(pool_.size() - 1);
}

bool OrderBook::add(OrderId id, Side side, Price price, Quantity quantity, OwnerId owner) {
    if (quantity == 0) return false;
    std::uint32_t* slot = index_.try_insert(id);
    if (slot == nullptr) return false;

    Levels& book = levels(side);
    const std::size_t i = lower_level(book, side, price);
    if (i == book.size() || book[i].price != price) {
        book.insert(book.begin() + static_cast<std::ptrdiff_t>(i),
                    Level{price, 0, 0, kNull, kNull});
    }
    Level& level = book[i];

    const NodeIndex n = allocate();
    pool_[n] = Node{id, quantity, owner, price, level.tail, kNull, side};
    if (level.tail != kNull) {
        pool_[level.tail].next = n;
    } else {
        level.head = n;
    }
    level.tail = n;
    level.quantity += quantity;
    ++level.count;
    *slot = n;  // nothing below touches index_, so the slot pointer is still valid
    return true;
}

Quantity OrderBook::reduce(OrderId id, Quantity quantity) {
    const NodeIndex n = index_.find(id);
    if (n == kNull) return 0;

    Node& node = pool_[n];
    if (quantity >= node.quantity) {
        const Quantity removed = node.quantity;
        erase(n);
        return removed;
    }
    node.quantity -= quantity;
    Levels& book = levels(node.side);
    book[lower_level(book, node.side, node.price)].quantity -= quantity;
    return quantity;
}

bool OrderBook::remove(OrderId id) {
    const NodeIndex n = index_.find(id);
    if (n == kNull) return false;
    erase(n);
    return true;
}

bool OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
    if (quantity == 0) return false;
    const NodeIndex n = index_.find(old_id);
    if (n == kNull) return false;
    if (new_id != old_id && index_.contains(new_id)) return false;

    const Node& node = pool_[n];
    const Side side = node.side;
    const OwnerId owner = node.owner;
    erase(n);
    return add(new_id, side, price, quantity, owner);
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const {
    const NodeIndex n = index_.find(id);
    if (n == kNull) return std::nullopt;
    const Node& node = pool_[n];
    return OrderInfo{id, node.side, node.price, node.quantity, node.owner};
}

std::optional<LevelInfo> OrderBook::best(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const Level& level = book.back();
    return LevelInfo{level.price, level.quantity, level.count};
}

std::optional<OrderInfo> OrderBook::front(Side side) const {
    const Levels& book = levels(side);
    if (book.empty()) return std::nullopt;
    const Node& node = pool_[book.back().head];
    return OrderInfo{node.id, side, node.price, node.quantity, node.owner};
}

void OrderBook::fill_front(Side side, Quantity quantity) {
    Level& level = levels(side).back();
    Node& node = pool_[level.head];
    if (quantity >= node.quantity) {
        erase(level.head);
        return;
    }
    node.quantity -= quantity;
    level.quantity -= quantity;
}

void OrderBook::erase(NodeIndex n) {
    Node& node = pool_[n];
    Levels& book = levels(node.side);
    const std::size_t i = lower_level(book, node.side, node.price);
    Level& level = book[i];

    if (node.prev != kNull) {
        pool_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNull) {
        pool_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    level.quantity -= node.quantity;
    if (--level.count == 0) book.erase(book.begin() + static_cast<std::ptrdiff_t>(i));

    index_.erase(node.id);
    node.next = free_head_;
    free_head_ = n;
}

}  // namespace lob
