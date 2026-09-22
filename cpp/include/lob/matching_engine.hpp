#pragma once

// Price-time priority matching engine for a single instrument.
//
// An incoming order trades against the opposite side of the book, best price first and oldest
// order first within a price, at the resting order's price. Whatever does not fill rests on
// the book (limit) or is cancelled (market, immediate-or-cancel).
//
// Fills are delivered to a callback passed to submit(). It is a template parameter, so the
// call is inlined: there is no std::function or virtual call on the matching path.

#include <algorithm>
#include <cstdint>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

enum class OrderType : std::uint8_t {
    kLimit,              // trades up to its limit price, then rests the remainder
    kMarket,             // trades at any price, never rests
    kImmediateOrCancel,  // trades up to its limit price, never rests
};

// What to do when an incoming order would trade with a resting order from the same owner.
enum class SelfTradePrevention : std::uint8_t {
    kCancelIncoming,  // cancel the rest of the incoming order; the resting order is untouched
    kCancelResting,   // cancel the resting order and keep matching
};

struct NewOrder {
    OrderId id;
    Side side;
    OrderType type;
    Price price;  // ignored for market orders
    Quantity quantity;
    OwnerId owner = kNoOwner;
};

struct Fill {
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;
    Price price;  // always the resting order's price
    Quantity quantity;

    friend bool operator==(const Fill&, const Fill&) = default;
};

enum class SubmitStatus : std::uint8_t {
    kAccepted,
    kRejectedDuplicateId,  // the id is already resting on the book
    kRejectedZeroQuantity,
    kRejectedZeroPrice,  // limit and IOC orders need a price
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::kAccepted;
    Quantity filled = 0;     // shares traded
    Quantity rested = 0;     // shares added to the book
    Quantity cancelled = 0;  // shares of the incoming order cancelled (unfilled market/IOC or STP)
    std::uint32_t fill_count = 0;
    std::uint32_t self_trade_cancels = 0;  // resting orders removed by kCancelResting

    [[nodiscard]] bool accepted() const noexcept { return status == SubmitStatus::kAccepted; }
};

class MatchingEngine {
public:
    explicit MatchingEngine(SelfTradePrevention stp = SelfTradePrevention::kCancelIncoming)
        : stp_(stp) {}

    // Matches `order` against the book, calling on_fill(const Fill&) for every trade.
    template <class OnFill>
    SubmitResult submit(const NewOrder& order, OnFill&& on_fill);

    SubmitResult submit(const NewOrder& order) {
        return submit(order, [](const Fill&) {});
    }

    // Cancels whatever remains of a resting order. Returns false if it is not on the book,
    // including when it has already been completely filled.
    bool cancel(OrderId id) { return book_.remove(id); }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

private:
    static bool crosses(Side side, Price limit, Price resting) noexcept {
        return side == Side::kBuy ? limit >= resting : limit <= resting;
    }

    OrderBook book_;
    SelfTradePrevention stp_;
};

template <class OnFill>
SubmitResult MatchingEngine::submit(const NewOrder& order, OnFill&& on_fill) {
    SubmitResult result;
    if (order.quantity == 0) {
        result.status = SubmitStatus::kRejectedZeroQuantity;
        return result;
    }
    if (order.type != OrderType::kMarket && order.price == 0) {
        result.status = SubmitStatus::kRejectedZeroPrice;
        return result;
    }
    if (book_.find(order.id)) {
        result.status = SubmitStatus::kRejectedDuplicateId;
        return result;
    }

    const Side contra = opposite(order.side);
    Quantity remaining = order.quantity;
    bool stopped_by_self_trade = false;

    while (remaining > 0) {
        const auto resting = book_.front(contra);
        if (!resting) break;
        if (order.type != OrderType::kMarket && !crosses(order.side, order.price, resting->price))
            break;

        if (order.owner != kNoOwner && resting->owner == order.owner) {
            if (stp_ == SelfTradePrevention::kCancelResting) {
                book_.remove(resting->id);
                ++result.self_trade_cancels;
                continue;
            }
            stopped_by_self_trade = true;
            break;
        }

        const Quantity traded = std::min(remaining, resting->quantity);
        book_.fill_front(contra, traded);
        remaining -= traded;
        result.filled += traded;
        ++result.fill_count;
        on_fill(Fill{order.id, resting->id, order.side, resting->price, traded});
    }

    if (remaining > 0) {
        if (order.type == OrderType::kLimit && !stopped_by_self_trade) {
            book_.add(order.id, order.side, order.price, remaining, order.owner);
            result.rested = remaining;
        } else {
            result.cancelled = remaining;
        }
    }
    return result;
}

}  // namespace lob
