#include "lob/replayer.hpp"

#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "lob/itch.hpp"
#include "lob/itch_parser.hpp"
#include "lob/order_book.hpp"

namespace lob {

namespace {

constexpr std::size_t kLocates = 65536;

// Refuse snapshot requests whose output would not reasonably fit in memory (e.g. 1 ms samples
// for many symbols over a whole day) instead of being killed by the OOM killer mid-allocation.
constexpr double kMaxSnapshotBytes = 4.0 * 1024 * 1024 * 1024;

constexpr Side to_side(char c) noexcept { return c == 'B' ? Side::kBuy : Side::kSell; }

void check_parse(const itch::ParseResult& result) {
    if (!result.ok()) {
        throw std::invalid_argument("malformed ITCH file at byte " +
                                    std::to_string(result.bytes_consumed) + ": " +
                                    std::string(itch::to_string(result.error)));
    }
}

// First pass: stock directory and time range.
struct DirectoryScan {
    std::unordered_map<std::string, std::uint16_t> locates;
    std::vector<std::string> in_file_order;
    std::uint64_t first_ts = 0;
    std::uint64_t last_ts = 0;
    bool seen = false;

    void on_message(char, std::span<const std::uint8_t> payload) {
        if (payload.size() < 11) return;
        const std::uint64_t ts = itch::read_be48(payload.data() + 5);
        if (!seen) {
            first_ts = ts;
            seen = true;
        }
        last_ts = ts;
    }

    void on_stock_directory(const itch::StockDirectory& m) {
        std::string symbol(itch::symbol_view(m.stock));
        if (locates.emplace(symbol, m.stock_locate).second) in_file_order.push_back(symbol);
    }
};

// Replays order messages into one book per tracked symbol, optionally sampling snapshots on a
// time grid and recording executions.
class Session {
public:
    Session(const std::vector<std::int32_t>& slot_of_locate, std::size_t symbols)
        : slot_of_locate_(slot_of_locate), books_(symbols) {}

    // ---- Snapshot configuration ----
    void sample(SnapshotData* out, std::uint64_t start, std::uint64_t interval) {
        snapshots_ = out;
        next_sample_ = start;
        interval_ = interval;
        samples_ = out->rows / books_.size();
    }
    void record_trades(TradeData* out) { trades_ = out; }

    // Emits any samples still due after the last message.
    void finish() {
        if (snapshots_ == nullptr) return;
        while (sample_index_ < samples_) emit(next_sample_);
    }

    // ---- itch::parse() callbacks ----
    void on_message(char, std::span<const std::uint8_t> payload) {
        if (snapshots_ == nullptr || payload.size() < 11) return;
        // Messages at exactly a sample time are applied before that sample is taken, so emit
        // only the samples strictly before this message's timestamp.
        const std::uint64_t ts = itch::read_be48(payload.data() + 5);
        while (next_sample_ < ts && sample_index_ < samples_) {
            emit(next_sample_);
        }
    }

    void on_add_order(const itch::AddOrder& m) {
        if (OrderBook* b = book(m.stock_locate)) {
            b->add(m.order_ref, to_side(m.side), m.price, m.shares);
        }
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        OrderBook* b = book(m.stock_locate);
        if (b == nullptr) return;
        if (trades_ != nullptr) {
            if (const auto order = b->find(m.order_ref)) {
                record(m, m.stock_locate, order->price, m.executed_shares,
                       aggressor_against(order->side), m.match_number, 'E', true);
            }
        }
        b->reduce(m.order_ref, m.executed_shares);
    }

    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        OrderBook* b = book(m.stock_locate);
        if (b == nullptr) return;
        if (trades_ != nullptr) {
            if (const auto order = b->find(m.order_ref)) {
                record(m, m.stock_locate, m.execution_price, m.executed_shares,
                       aggressor_against(order->side), m.match_number, 'C', m.printable == 'Y');
            }
        }
        b->reduce(m.order_ref, m.executed_shares);
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        if (OrderBook* b = book(m.stock_locate)) b->reduce(m.order_ref, m.cancelled_shares);
    }

    void on_order_delete(const itch::OrderDelete& m) {
        if (OrderBook* b = book(m.stock_locate)) b->remove(m.order_ref);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        if (OrderBook* b = book(m.stock_locate)) {
            b->replace(m.original_order_ref, m.new_order_ref, m.price, m.shares);
        }
    }

    // A non-displayed order traded: no visible book change. `side` is the resting order's.
    void on_trade(const itch::Trade& m) {
        if (trades_ != nullptr && slot(m.stock_locate) >= 0) {
            record(m, m.stock_locate, m.price, m.shares, aggressor_against(to_side(m.side)),
                   m.match_number, 'P', true);
        }
    }

    void on_cross_trade(const itch::CrossTrade& m) {
        if (trades_ != nullptr && slot(m.stock_locate) >= 0) {
            record(m, m.stock_locate, m.cross_price, static_cast<std::int64_t>(m.shares), 0,
                   m.match_number, 'Q', true);
        }
    }

private:
    static std::int8_t aggressor_against(Side resting) noexcept {
        return resting == Side::kBuy ? std::int8_t{-1} : std::int8_t{1};
    }

    std::int32_t slot(std::uint16_t locate) const noexcept { return slot_of_locate_[locate]; }

    OrderBook* book(std::uint16_t locate) noexcept {
        const std::int32_t s = slot(locate);
        return s < 0 ? nullptr : &books_[static_cast<std::size_t>(s)];
    }

    void record(const itch::MessageHeader& h, std::uint16_t locate, std::int64_t price,
                std::int64_t shares, std::int8_t side, std::uint64_t match, char kind,
                bool printable) {
        trades_->timestamp.push_back(static_cast<std::int64_t>(h.timestamp_ns));
        trades_->symbol.push_back(slot(locate));
        trades_->price.push_back(price);
        trades_->shares.push_back(shares);
        trades_->side.push_back(side);
        trades_->match_number.push_back(match);
        trades_->kind.push_back(static_cast<std::uint8_t>(kind));
        trades_->printable.push_back(printable ? 1 : 0);
    }

    void emit(std::uint64_t t) {
        SnapshotData& out = *snapshots_;
        const std::size_t depth = out.depth;
        for (std::size_t s = 0; s < books_.size(); ++s) {
            const std::size_t row = sample_index_ * books_.size() + s;
            out.timestamp[row] = static_cast<std::int64_t>(t);
            out.symbol[row] = static_cast<std::int32_t>(s);
            std::size_t level = 0;
            books_[s].for_each_level(Side::kBuy, depth, [&](const LevelInfo& l) {
                out.bid_price[row * depth + level] = l.price;
                out.bid_size[row * depth + level] = l.quantity;
                ++level;
            });
            level = 0;
            books_[s].for_each_level(Side::kSell, depth, [&](const LevelInfo& l) {
                out.ask_price[row * depth + level] = l.price;
                out.ask_size[row * depth + level] = l.quantity;
                ++level;
            });
        }
        ++sample_index_;
        next_sample_ = t + interval_;
    }

    const std::vector<std::int32_t>& slot_of_locate_;
    std::vector<OrderBook> books_;

    SnapshotData* snapshots_ = nullptr;
    std::uint64_t next_sample_ = 0;
    std::uint64_t interval_ = 1;
    std::size_t samples_ = 0;
    std::size_t sample_index_ = 0;

    TradeData* trades_ = nullptr;
};

}  // namespace

Replayer::Replayer(const std::filesystem::path& path, std::vector<std::string> symbols)
    : file_(path), slot_of_locate_(kLocates, -1) {
    DirectoryScan scan;
    check_parse(itch::parse(file_.bytes(), scan));
    first_ts_ = scan.first_ts;
    last_ts_ = scan.last_ts;

    symbols_ = symbols.empty() ? std::move(scan.in_file_order) : std::move(symbols);
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        const auto it = scan.locates.find(symbols_[i]);
        if (it == scan.locates.end()) {
            throw std::invalid_argument("symbol not in file: " + symbols_[i]);
        }
        if (slot_of_locate_[it->second] >= 0) {
            throw std::invalid_argument("duplicate symbol: " + symbols_[i]);
        }
        slot_of_locate_[it->second] = static_cast<std::int32_t>(i);
    }
}

SnapshotData Replayer::snapshots(std::uint64_t interval_ns, std::size_t depth,
                                 std::optional<std::uint64_t> start_ns,
                                 std::optional<std::uint64_t> end_ns) const {
    if (interval_ns == 0) throw std::invalid_argument("interval_ns must be positive");
    if (depth == 0) throw std::invalid_argument("depth must be positive");
    const std::uint64_t start =
        start_ns.value_or((first_ts_ + interval_ns - 1) / interval_ns * interval_ns);
    const std::uint64_t end = end_ns.value_or(last_ts_);
    if (end < start) throw std::invalid_argument("end_ns is before start_ns");

    // Preallocate every buffer to its final size: the grid is known before the replay.
    SnapshotData out;
    const std::size_t samples = static_cast<std::size_t>((end - start) / interval_ns) + 1;
    out.rows = samples * symbols_.size();
    const double bytes = static_cast<double>(out.rows) *
                         static_cast<double>(sizeof(std::int64_t) + sizeof(std::int32_t) +
                                             4 * depth * sizeof(std::int64_t));
    if (bytes > kMaxSnapshotBytes) {
        throw std::invalid_argument(
            "snapshot request would allocate " + std::to_string(bytes / 1e9) + " GB (" +
            std::to_string(out.rows) + " rows); use a larger interval, a shorter window, " +
            "fewer symbols or less depth");
    }
    out.depth = depth;
    out.timestamp.resize(out.rows);
    out.symbol.resize(out.rows);
    out.bid_price.resize(out.rows * depth);
    out.bid_size.resize(out.rows * depth);
    out.ask_price.resize(out.rows * depth);
    out.ask_size.resize(out.rows * depth);
    if (out.rows == 0) return out;

    Session session(slot_of_locate_, symbols_.size());
    session.sample(&out, start, interval_ns);
    check_parse(itch::parse(file_.bytes(), session));
    session.finish();
    return out;
}

TradeData Replayer::trades() const {
    TradeData out;
    Session session(slot_of_locate_, symbols_.size());
    session.record_trades(&out);
    check_parse(itch::parse(file_.bytes(), session));
    return out;
}

}  // namespace lob
