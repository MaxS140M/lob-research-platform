#pragma once

#include <cstddef>

namespace lob {

// Limit order book for a single instrument.
// Placeholder interface: the full implementation lands in Phase 2.
class OrderBook {
public:
    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::size_t order_count_ = 0;
};

}  // namespace lob
