#pragma once

// Rebuilds every instrument's order book from an ITCH 5.0 stream.
//
// BookBuilder is an itch::parse() handler. Books are indexed by stock locate code, which every
// ITCH message carries, so executions, cancels and deletes (which only carry an order
// reference) are routed straight to the right book without a global order lookup.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lob/itch.hpp"
#include "lob/order_book.hpp"

namespace lob {

class BookBuilder {
public:
    // Messages that did not apply cleanly. All zero for a complete, well-formed day.
    struct Anomalies {
        std::uint64_t duplicate_adds = 0;   // add for an order reference that is already live
        std::uint64_t unknown_orders = 0;   // execute/cancel/delete/replace of an unknown order
        std::uint64_t over_reductions = 0;  // executed/cancelled more shares than were resting
        std::uint64_t failed_replaces = 0;  // replace whose new reference is already live

        [[nodiscard]] std::uint64_t total() const noexcept {
            return duplicate_adds + unknown_orders + over_reductions + failed_replaces;
        }
    };

    // ---- itch::parse() callbacks ----
    void on_stock_directory(const itch::StockDirectory& m);
    void on_add_order(const itch::AddOrder& m);  // also receives 'F' (MPID) adds
    void on_order_executed(const itch::OrderExecuted& m);
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m);
    void on_order_cancel(const itch::OrderCancel& m);
    void on_order_delete(const itch::OrderDelete& m);
    void on_order_replace(const itch::OrderReplace& m);

    // ---- Queries ----
    // The book for a locate code; an empty book if nothing was ever added for it.
    [[nodiscard]] const OrderBook& book(std::uint16_t locate) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> locate(std::string_view symbol) const;
    [[nodiscard]] std::string_view symbol(std::uint16_t locate) const noexcept;

    [[nodiscard]] const Anomalies& anomalies() const noexcept { return anomalies_; }
    [[nodiscard]] std::size_t book_count() const noexcept { return books_.size(); }
    [[nodiscard]] std::size_t total_orders() const noexcept;

private:
    OrderBook& book_for_add(std::uint16_t locate);
    OrderBook* existing_book(std::uint16_t locate) noexcept {
        return locate < books_.size() ? &books_[locate] : nullptr;
    }
    void reduce(std::uint16_t locate, OrderId id, Quantity quantity);

    std::vector<OrderBook> books_;
    std::vector<std::string> symbols_;
    std::unordered_map<std::string, std::uint16_t> locates_;
    Anomalies anomalies_;
};

}  // namespace lob
