// Turns parsed ITCH messages into order book operations, one book per stock. Anything that
// doesn't apply cleanly (e.g. a cancel for an unknown order) is counted, not fatal.

#include "lob/book_builder.hpp"

#include <utility>

namespace lob {

namespace {

constexpr Side to_side(char c) noexcept { return c == 'B' ? Side::kBuy : Side::kSell; }

const OrderBook kEmptyBook;

}  // namespace

void BookBuilder::on_stock_directory(const itch::StockDirectory& m) {
    if (m.stock_locate >= symbols_.size()) symbols_.resize(std::size_t{m.stock_locate} + 1);
    std::string symbol(itch::symbol_view(m.stock));
    locates_[symbol] = m.stock_locate;
    symbols_[m.stock_locate] = std::move(symbol);
}

void BookBuilder::on_add_order(const itch::AddOrder& m) {
    if (!book_for_add(m.stock_locate).add(m.order_ref, to_side(m.side), m.price, m.shares)) {
        ++anomalies_.duplicate_adds;
    }
}

void BookBuilder::on_order_executed(const itch::OrderExecuted& m) {
    reduce(m.stock_locate, m.order_ref, m.executed_shares);
}

void BookBuilder::on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
    reduce(m.stock_locate, m.order_ref, m.executed_shares);
}

void BookBuilder::on_order_cancel(const itch::OrderCancel& m) {
    reduce(m.stock_locate, m.order_ref, m.cancelled_shares);
}

void BookBuilder::on_order_delete(const itch::OrderDelete& m) {
    OrderBook* book = existing_book(m.stock_locate);
    if (book == nullptr || !book->remove(m.order_ref)) ++anomalies_.unknown_orders;
}

void BookBuilder::on_order_replace(const itch::OrderReplace& m) {
    OrderBook* book = existing_book(m.stock_locate);
    if (book != nullptr &&
        book->replace(m.original_order_ref, m.new_order_ref, m.price, m.shares)) {
        return;
    }
    // A failed replace leaves the book unchanged, so the original can still be looked up.
    if (book == nullptr || !book->find(m.original_order_ref)) {
        ++anomalies_.unknown_orders;
    } else {
        ++anomalies_.failed_replaces;
    }
}

void BookBuilder::reduce(std::uint16_t locate, OrderId id, Quantity quantity) {
    OrderBook* book = existing_book(locate);
    if (book == nullptr) {
        ++anomalies_.unknown_orders;
        return;
    }
    const Quantity removed = book->reduce(id, quantity);
    if (removed == 0) {
        ++anomalies_.unknown_orders;
    } else if (removed < quantity) {
        ++anomalies_.over_reductions;
    }
}

OrderBook& BookBuilder::book_for_add(std::uint16_t locate) {
    if (locate >= books_.size()) books_.resize(std::size_t{locate} + 1);
    return books_[locate];
}

const OrderBook& BookBuilder::book(std::uint16_t locate) const noexcept {
    return locate < books_.size() ? books_[locate] : kEmptyBook;
}

std::optional<std::uint16_t> BookBuilder::locate(std::string_view symbol) const {
    const auto it = locates_.find(std::string(symbol));
    if (it == locates_.end()) return std::nullopt;
    return it->second;
}

std::string_view BookBuilder::symbol(std::uint16_t locate) const noexcept {
    return locate < symbols_.size() ? std::string_view(symbols_[locate]) : std::string_view();
}

std::size_t BookBuilder::total_orders() const noexcept {
    std::size_t n = 0;
    for (const auto& b : books_) n += b.order_count();
    return n;
}

}  // namespace lob
