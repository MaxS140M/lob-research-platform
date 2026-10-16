#include "lob/order_book.hpp"

namespace lob {

std::size_t OrderBook::order_count() const noexcept { return order_count_; }

bool OrderBook::empty() const noexcept { return order_count_ == 0; }

}  // namespace lob
