#pragma once

#include <cstdint>

namespace lob {

// Parser for NASDAQ TotalView-ITCH 5.0 binary messages.
// Placeholder interface: the full implementation lands in Phase 1.
class ItchParser {
public:
    [[nodiscard]] std::uint64_t messages_parsed() const noexcept;

private:
    std::uint64_t messages_parsed_ = 0;
};

}  // namespace lob
