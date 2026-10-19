#include "lob/replayer.hpp"

#include <algorithm>
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

// Replays one symbol's book and records every event touching it, with queue priorities.
class EventSession {
public:
    EventSession(std::uint16_t locate, std::size_t depth, std::uint64_t start, std::uint64_t end,
                 EventData& out)
        : locate_(locate), depth_(depth), start_(start), end_(end), out_(out) {}

    void on_add_order(const itch::AddOrder& m) {
        if (m.stock_locate != locate_) return;
        ++seq_;
        if (!book_.add(m.order_ref, to_side(m.side), m.price, m.shares)) return;
        priority_[m.order_ref] = seq_;
        emit(m, 'A', sign(to_side(m.side)), m.price, m.shares, 0, current_seq());
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        reduce(m, 'E', m.order_ref, m.executed_shares, std::nullopt);
    }

    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        reduce(m, 'C', m.order_ref, m.executed_shares, m.execution_price);
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        reduce(m, 'X', m.order_ref, m.cancelled_shares, std::nullopt);
    }

    void on_order_delete(const itch::OrderDelete& m) {
        if (m.stock_locate != locate_) return;
        ++seq_;
        const auto order = book_.find(m.order_ref);
        if (!order) return;
        const std::int64_t priority = take_priority(m.order_ref);
        book_.remove(m.order_ref);
        emit(m, 'D', sign(order->side), order->price, order->quantity, 0, priority);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        if (m.stock_locate != locate_) return;
        ++seq_;
        const auto old = book_.find(m.original_order_ref);
        if (!old) return;
        if (!book_.replace(m.original_order_ref, m.new_order_ref, m.price, m.shares)) return;
        const std::int64_t old_priority = take_priority(m.original_order_ref);
        priority_[m.new_order_ref] = seq_;  // a replace loses time priority
        emit(m, 'U', sign(old->side), m.price, m.shares, 0, current_seq(), old->price,
             old->quantity, old_priority);
    }

    void on_trade(const itch::Trade& m) {
        if (m.stock_locate != locate_) return;
        ++seq_;
        emit(m, 'P', sign(to_side(m.side)), m.price, m.shares, m.price, 0);
    }

    void on_cross_trade(const itch::CrossTrade& m) {
        if (m.stock_locate != locate_) return;
        ++seq_;
        emit(m, 'Q', 0, m.cross_price, static_cast<std::int64_t>(m.shares), m.cross_price, 0);
    }

private:
    static std::int8_t sign(Side s) noexcept { return s == Side::kBuy ? 1 : -1; }

    std::int64_t current_seq() const noexcept { return static_cast<std::int64_t>(seq_); }

    std::int64_t take_priority(OrderId id) {
        const auto it = priority_.find(id);
        if (it == priority_.end()) return 0;
        const auto p = static_cast<std::int64_t>(it->second);
        priority_.erase(it);
        return p;
    }

    void reduce(const itch::MessageHeader& h, char kind, OrderId id, Quantity shares,
                std::optional<Price> execution_price) {
        if (h.stock_locate != locate_) return;
        ++seq_;
        const auto order = book_.find(id);
        if (!order) return;
        const auto it = priority_.find(id);
        const std::int64_t priority =
            it == priority_.end() ? 0 : static_cast<std::int64_t>(it->second);
        const Quantity removed = book_.reduce(id, shares);
        if (!book_.find(id)) priority_.erase(id);
        const bool is_execution = kind == 'E' || kind == 'C';
        const std::int64_t trade_price = is_execution ? execution_price.value_or(order->price) : 0;
        emit(h, kind, sign(order->side), order->price, removed, trade_price, priority);
    }

    void emit(const itch::MessageHeader& h, char kind, std::int8_t side, std::int64_t price,
              std::int64_t shares, std::int64_t trade_price, std::int64_t priority,
              std::int64_t old_price = 0, std::int64_t old_shares = 0,
              std::int64_t old_priority = 0) {
        if (h.timestamp_ns < start_ || h.timestamp_ns > end_) return;
        out_.timestamp.push_back(static_cast<std::int64_t>(h.timestamp_ns));
        out_.seq.push_back(static_cast<std::int64_t>(seq_));
        out_.kind.push_back(static_cast<std::uint8_t>(kind));
        out_.side.push_back(side);
        out_.price.push_back(price);
        out_.shares.push_back(shares);
        out_.trade_price.push_back(trade_price);
        out_.priority.push_back(priority);
        out_.old_price.push_back(old_price);
        out_.old_shares.push_back(old_shares);
        out_.old_priority.push_back(old_priority);

        const std::size_t base = out_.bid_price.size();
        out_.bid_price.resize(base + depth_, 0);
        out_.bid_size.resize(base + depth_, 0);
        out_.ask_price.resize(base + depth_, 0);
        out_.ask_size.resize(base + depth_, 0);
        std::size_t level = 0;
        book_.for_each_level(Side::kBuy, depth_, [&](const LevelInfo& l) {
            out_.bid_price[base + level] = l.price;
            out_.bid_size[base + level] = l.quantity;
            ++level;
        });
        level = 0;
        book_.for_each_level(Side::kSell, depth_, [&](const LevelInfo& l) {
            out_.ask_price[base + level] = l.price;
            out_.ask_size[base + level] = l.quantity;
            ++level;
        });
    }

    std::uint16_t locate_;
    std::size_t depth_;
    std::uint64_t start_;
    std::uint64_t end_;
    EventData& out_;
    OrderBook book_;
    std::unordered_map<OrderId, std::uint64_t> priority_;
    std::uint64_t seq_ = 0;
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

EventData Replayer::events(const std::string& symbol, std::size_t depth,
                           std::optional<std::uint64_t> start_ns,
                           std::optional<std::uint64_t> end_ns) const {
    if (depth == 0) throw std::invalid_argument("depth must be positive");
    const auto it = std::find(symbols_.begin(), symbols_.end(), symbol);
    if (it == symbols_.end()) {
        throw std::invalid_argument("symbol not in this Replayer: " + symbol);
    }
    std::uint16_t locate = 0;
    const auto slot = static_cast<std::int32_t>(it - symbols_.begin());
    for (std::size_t l = 0; l < slot_of_locate_.size(); ++l) {
        if (slot_of_locate_[l] == slot) locate = static_cast<std::uint16_t>(l);
    }

    EventData out;
    out.depth = depth;
    EventSession session(locate, depth, start_ns.value_or(0), end_ns.value_or(UINT64_MAX), out);
    check_parse(itch::parse(file_.bytes(), session));
    return out;
}

}  // namespace lob
