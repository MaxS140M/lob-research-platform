// book_replay: rebuilds every order book from a full ITCH 5.0 day and checks it.
//
//   usage: book_replay <file> [symbols-to-show]
//
// Checks: every execute/cancel/delete/replace refers to a live order, no order is reduced
// below zero, and books are never locked or crossed (best bid >= best ask), split by session.
// Prints top of book at the 16:00 close for the most active symbols, and replay throughput.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>
#include <vector>

#include "lob/book_builder.hpp"
#include "lob/itch.hpp"
#include "lob/itch_parser.hpp"
#include "lob/mapped_file.hpp"

namespace itch = lob::itch;

namespace {

enum Session : std::size_t { kPreMarket, kRegular, kPostMarket, kSessionCount };
constexpr std::array<const char*, kSessionCount> kSessionNames = {"pre-market", "regular",
                                                                  "post-market"};

struct Snapshot {
    std::uint16_t locate;
    std::optional<lob::LevelInfo> bid;
    std::optional<lob::LevelInfo> ask;
    std::size_t bid_levels;
    std::size_t ask_levels;
    std::size_t orders;
};

// Wraps BookBuilder and checks the touched book after every message that changes it.
class CheckedReplay : public lob::BookBuilder {
public:
    std::vector<std::uint64_t> messages_per_locate = std::vector<std::uint64_t>(65536, 0);
    std::array<std::uint64_t, kSessionCount> locked{};
    std::array<std::uint64_t, kSessionCount> crossed{};
    std::array<std::uint64_t, kSessionCount> checks{};
    std::vector<Snapshot> close_snapshot;
    std::size_t peak_orders = 0;
    std::size_t live_orders = 0;
    std::size_t symbols_to_show = 10;

    void on_message(char, std::span<const std::uint8_t> payload) {
        if (payload.size() >= 3) ++messages_per_locate[itch::read_be16(payload.data() + 1)];
    }

    void on_system_event(const itch::SystemEvent& e) {
        if (e.event_code == itch::event_code::kStartOfMarketHours) session_ = kRegular;
        if (e.event_code == itch::event_code::kEndOfMarketHours) {
            session_ = kPostMarket;
            take_close_snapshot();
        }
    }

    void on_add_order(const itch::AddOrder& m) {
        BookBuilder::on_add_order(m);
        ++live_orders;
        peak_orders = std::max(peak_orders, live_orders);
        check(m.stock_locate);
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        track_removal(m.stock_locate, [&] { BookBuilder::on_order_executed(m); });
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        track_removal(m.stock_locate, [&] { BookBuilder::on_order_executed_with_price(m); });
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        track_removal(m.stock_locate, [&] { BookBuilder::on_order_cancel(m); });
    }
    void on_order_delete(const itch::OrderDelete& m) {
        track_removal(m.stock_locate, [&] { BookBuilder::on_order_delete(m); });
    }
    void on_order_replace(const itch::OrderReplace& m) {
        BookBuilder::on_order_replace(m);
        check(m.stock_locate);
    }

private:
    template <class F>
    void track_removal(std::uint16_t locate, F&& apply) {
        const std::size_t before = book(locate).order_count();
        apply();
        live_orders -= before - book(locate).order_count();
        check(locate);
    }

    void check(std::uint16_t locate) {
        const lob::OrderBook& b = book(locate);
        ++checks[session_];
        const auto bid = b.best_bid();
        const auto ask = b.best_ask();
        if (!bid || !ask) return;
        if (bid->price == ask->price) ++locked[session_];
        if (bid->price > ask->price) ++crossed[session_];
    }

    void take_close_snapshot() {
        std::vector<std::uint16_t> locates;
        for (std::size_t i = 0; i < messages_per_locate.size(); ++i) {
            if (messages_per_locate[i] > 0) locates.push_back(static_cast<std::uint16_t>(i));
        }
        const std::size_t n = std::min(symbols_to_show, locates.size());
        std::partial_sort(locates.begin(), locates.begin() + static_cast<std::ptrdiff_t>(n),
                          locates.end(), [&](std::uint16_t a, std::uint16_t b) {
                              return messages_per_locate[a] > messages_per_locate[b];
                          });
        for (std::size_t i = 0; i < n; ++i) {
            const lob::OrderBook& b = book(locates[i]);
            close_snapshot.push_back({locates[i], b.best_bid(), b.best_ask(),
                                      b.level_count(lob::Side::kBuy),
                                      b.level_count(lob::Side::kSell), b.order_count()});
        }
    }

    Session session_ = kPreMarket;
};

void print_price(std::optional<lob::LevelInfo> level) {
    if (!level) {
        std::printf("%22s", "-");
        return;
    }
    std::printf("%8u @ %8u.%04u", level->quantity, level->price / itch::kPriceScale,
                level->price % itch::kPriceScale);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <itch-5.0-file> [symbols-to-show]\n", argv[0]);
        return 2;
    }

    try {
        const lob::MappedFile file(argv[1]);
        CheckedReplay replay;
        if (argc == 3) replay.symbols_to_show = std::strtoul(argv[2], nullptr, 10);

        const auto start = std::chrono::steady_clock::now();
        const itch::ParseResult result = itch::parse(file.bytes(), replay);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!result.ok()) {
            std::fprintf(stderr, "parse error at byte %zu\n", result.bytes_consumed);
            return 1;
        }

        const auto& a = replay.anomalies();
        std::printf(
            "Replayed %llu messages into %zu books in %.3f s (%.1f M msg/s, with checks)\n\n",
            static_cast<unsigned long long>(result.messages), replay.book_count(), seconds,
            static_cast<double>(result.messages) / seconds / 1e6);

        std::printf("Anomalies\n");
        std::printf("  unknown order references: %llu\n",
                    static_cast<unsigned long long>(a.unknown_orders));
        std::printf("  duplicate adds:           %llu\n",
                    static_cast<unsigned long long>(a.duplicate_adds));
        std::printf("  over-reductions:          %llu\n",
                    static_cast<unsigned long long>(a.over_reductions));
        std::printf("  failed replaces:          %llu\n\n",
                    static_cast<unsigned long long>(a.failed_replaces));

        std::printf("Locked / crossed books after a book update\n");
        std::printf("  %-12s %14s %10s %10s\n", "session", "checks", "locked", "crossed");
        for (std::size_t s = 0; s < kSessionCount; ++s) {
            std::printf("  %-12s %14llu %10llu %10llu\n", kSessionNames[s],
                        static_cast<unsigned long long>(replay.checks[s]),
                        static_cast<unsigned long long>(replay.locked[s]),
                        static_cast<unsigned long long>(replay.crossed[s]));
        }

        std::printf("\nOrders resting: peak %zu, end of day %zu\n\n", replay.peak_orders,
                    replay.total_orders());

        std::printf("Top of book at the 16:00 close (most active symbols)\n");
        std::printf("  %-8s %22s   %22s %8s %8s %8s\n", "symbol", "bid (shares @ $)",
                    "ask (shares @ $)", "spread", "levels", "orders");
        for (const auto& s : replay.close_snapshot) {
            const auto sym = replay.symbol(s.locate);
            std::printf("  %-8.*s ", static_cast<int>(sym.size()), sym.data());
            print_price(s.bid);
            std::printf("   ");
            print_price(s.ask);
            if (s.bid && s.ask) {
                std::printf(" %8.4f", static_cast<double>(static_cast<std::int64_t>(s.ask->price) -
                                                          static_cast<std::int64_t>(s.bid->price)) /
                                          itch::kPriceScale);
            } else {
                std::printf(" %8s", "-");
            }
            std::printf(" %4zu/%-4zu %7zu\n", s.bid_levels, s.ask_levels, s.orders);
        }

        const bool clean = a.total() == 0 && replay.crossed[kRegular] == 0;
        return clean ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
