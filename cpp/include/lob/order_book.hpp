#pragma once

// Limit order book for a single instrument, with price-time priority.
//
// The book only stores resting orders; it does not match. Matching is done by
// MatchingEngine on top of the primitives here (front / fill_front), and ITCH replay drives
// the book directly, because the exchange has already done the matching.
//
// Layout (see docs/benchmarks.md for the version history):
// - Price levels live in one flat vector per side, sorted so the best price is at the back.
//   Top of book is back(), and level searches start there because almost all activity is
//   within a few levels of the touch.
// - Orders live in a pool (one contiguous vector with a free list), so steady-state adds do
//   not allocate. Each level's FIFO queue is an intrusive doubly linked list threaded through
//   the pool by 32-bit indices.
// - Order ids map to pool slots through an open-addressing hash table (OrderIndex).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/order_index.hpp"
#include "lob/types.hpp"

namespace lob {

class OrderBook {
public:
    OrderBook() = default;

    // Pre-sizes the order pool and index so the first `expected_orders` adds do not allocate.
    explicit OrderBook(std::size_t expected_orders);

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
    using NodeIndex = std::uint32_t;
    static constexpr NodeIndex kNull = UINT32_MAX;
    static_assert(kNull == OrderIndex::kMissing);

    // One resting order, linked into its level's queue (front is oldest).
    struct Node {
        OrderId id;
        Quantity quantity;
        OwnerId owner;
        Price price;
        NodeIndex prev;
        NodeIndex next;  // also links the free list
        Side side;
    };
    struct Level {
        Price price;
        Quantity quantity;
        std::uint32_t count;
        NodeIndex head;
        NodeIndex tail;
    };
    // Sorted worst to best: bids ascending, asks descending, so the best level is back().
    using Levels = std::vector<Level>;

    Levels& levels(Side side) noexcept { return side == Side::kBuy ? bids_ : asks_; }
    const Levels& levels(Side side) const noexcept { return side == Side::kBuy ? bids_ : asks_; }

    // True if `a` is a worse price than `b` on `side` (further from the touch).
    static bool worse(Side side, Price a, Price b) noexcept {
        return side == Side::kBuy ? a < b : a > b;
    }

    // Index of the first level whose price is not worse than `price`: the level at `price` if
    // there is one, otherwise where it would be inserted.
    static std::size_t lower_level(const Levels& levels, Side side, Price price) noexcept;

    NodeIndex allocate();
    void erase(NodeIndex n);  // unlinks, frees and unindexes a resting order

    Levels bids_;
    Levels asks_;
    std::vector<Node> pool_;
    NodeIndex free_head_ = kNull;
    OrderIndex index_;
};

template <class F>
void OrderBook::for_each_level(Side side, std::size_t max_levels, F&& f) const {
    const Levels& book = levels(side);
    const std::size_t n = std::min(max_levels, book.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Level& level = book[book.size() - 1 - i];
        f(LevelInfo{level.price, level.quantity, level.count});
    }
}

}  // namespace lob
