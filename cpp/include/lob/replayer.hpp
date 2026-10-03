#pragma once

// Research-facing replay of one ITCH 5.0 file for a set of symbols.
//
// Replayer is what the Python bindings expose. Each call replays the whole file through
// order books for the chosen symbols and returns column-oriented results in flat buffers, so
// they can be handed to NumPy without copying.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "lob/mapped_file.hpp"

namespace lob {

// Top-of-book snapshots sampled on a fixed time grid.
//
// Row r covers sample time timestamp[r] for symbol symbol[r]; rows are ordered by time, then by
// symbol in the order given to the Replayer. Each snapshot reflects every message with a
// timestamp <= its sample time. Level arrays are row-major [rows x depth], best level first;
// levels that do not exist are 0. Prices are integers with 4 implied decimals.
struct SnapshotData {
    std::size_t rows = 0;
    std::size_t depth = 0;
    std::vector<std::int64_t> timestamp;  // ns since midnight
    std::vector<std::int32_t> symbol;     // index into Replayer::symbols()
    std::vector<std::int64_t> bid_price;
    std::vector<std::int64_t> bid_size;
    std::vector<std::int64_t> ask_price;
    std::vector<std::int64_t> ask_size;
};

// Every execution in the file for the chosen symbols, in file order.
struct TradeData {
    std::vector<std::int64_t> timestamp;  // ns since midnight
    std::vector<std::int32_t> symbol;     // index into Replayer::symbols()
    std::vector<std::int64_t> price;      // 4 implied decimals
    std::vector<std::int64_t> shares;
    std::vector<std::int8_t> side;  // aggressor: +1 buyer-initiated, -1 seller-initiated, 0 cross
    std::vector<std::uint64_t> match_number;
    std::vector<std::uint8_t> kind;       // ITCH message type: 'E', 'C', 'P' or 'Q'
    std::vector<std::uint8_t> printable;  // 0 if the exchange marked it non-printable ('C' only)
};

class Replayer {
public:
    // Maps the file and reads its stock directory. An empty `symbols` list means every symbol
    // in the file. Throws std::system_error if the file cannot be read, and
    // std::invalid_argument for a symbol that is not in the file or a malformed file.
    Replayer(const std::filesystem::path& path, std::vector<std::string> symbols);

    // Samples every `interval_ns` from `start_ns` to `end_ns` inclusive. By default the grid
    // starts at the first multiple of the interval at or after the first message and ends at
    // the last message. Throws std::invalid_argument for a zero interval or depth, end < start,
    // or a request whose output would exceed 4 GiB.
    [[nodiscard]] SnapshotData snapshots(std::uint64_t interval_ns, std::size_t depth,
                                         std::optional<std::uint64_t> start_ns = std::nullopt,
                                         std::optional<std::uint64_t> end_ns = std::nullopt) const;

    [[nodiscard]] TradeData trades() const;

    [[nodiscard]] const std::vector<std::string>& symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::uint64_t first_timestamp() const noexcept { return first_ts_; }
    [[nodiscard]] std::uint64_t last_timestamp() const noexcept { return last_ts_; }

private:
    MappedFile file_;
    std::vector<std::string> symbols_;
    std::vector<std::int32_t> slot_of_locate_;  // locate -> index into symbols_, or -1
    std::uint64_t first_ts_ = 0;
    std::uint64_t last_ts_ = 0;
};

}  // namespace lob
