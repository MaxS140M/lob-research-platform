// itch_stats: parses a full ITCH 5.0 file and reports per-type message counts, sanity checks
// on timestamps and system events, and parse throughput.
//
//   usage: itch_stats <file>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_parser.hpp"
#include "lob/mapped_file.hpp"

namespace itch = lob::itch;

namespace {

struct StatsHandler {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t last_timestamp = 0;
    std::uint64_t first_timestamp = 0;
    std::uint64_t backward_steps = 0;  // messages whose timestamp precedes the previous one
    bool seen_any = false;
    std::vector<itch::SystemEvent> system_events;

    void on_message(char type, std::span<const std::uint8_t> payload) {
        ++counts[static_cast<std::uint8_t>(type)];
        // Every ITCH 5.0 message carries a timestamp at offset 5.
        if (payload.size() >= 11) {
            const std::uint64_t ts = itch::read_be48(payload.data() + 5);
            if (!seen_any) {
                first_timestamp = ts;
                seen_any = true;
            } else if (ts < last_timestamp) {
                ++backward_steps;
            }
            last_timestamp = ts;
        }
    }

    void on_system_event(const itch::SystemEvent& e) { system_events.push_back(e); }
};

// Decodes every order-book and trade message and folds its fields into a checksum, so the
// timing covers full field decoding and the compiler cannot discard the decoded values.
struct DecodeAllHandler {
    std::uint64_t checksum = 0;

    void mix(std::uint64_t v) { checksum = (checksum ^ v) * 0x100000001B3ULL; }

    void on_add_order(const itch::AddOrder& m) {
        mix(m.order_ref);
        mix(m.shares);
        mix(m.price);
        mix(static_cast<std::uint64_t>(m.side));
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        mix(m.order_ref);
        mix(m.executed_shares);
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        mix(m.order_ref);
        mix(m.executed_shares);
        mix(m.execution_price);
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        mix(m.order_ref);
        mix(m.cancelled_shares);
    }
    void on_order_delete(const itch::OrderDelete& m) { mix(m.order_ref); }
    void on_order_replace(const itch::OrderReplace& m) {
        mix(m.original_order_ref);
        mix(m.new_order_ref);
        mix(m.shares);
        mix(m.price);
    }
    void on_trade(const itch::Trade& m) {
        mix(m.shares);
        mix(m.price);
    }
    void on_cross_trade(const itch::CrossTrade& m) {
        mix(m.shares);
        mix(m.cross_price);
    }
};

void print_time(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000;
    std::printf("%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>((s / 60) % 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % 1'000'000'000));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-5.0-file>\n", argv[0]);
        return 2;
    }

    try {
        const lob::MappedFile file(argv[1]);

        StatsHandler stats;
        const auto start = std::chrono::steady_clock::now();
        const itch::ParseResult result = itch::parse(file.bytes(), stats);
        const auto stop = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(stop - start).count();

        // ---- Counts, most frequent first --------------------------------------
        std::vector<char> types;
        for (int t = 0; t < 256; ++t) {
            if (stats.counts[static_cast<std::size_t>(t)] > 0)
                types.push_back(static_cast<char>(t));
        }
        std::sort(types.begin(), types.end(), [&](char a, char b) {
            return stats.counts[static_cast<std::uint8_t>(a)] >
                   stats.counts[static_cast<std::uint8_t>(b)];
        });

        std::printf("File: %s (%.1f MB)\n\n", argv[1], static_cast<double>(file.size()) / 1e6);
        std::printf("%-4s %-32s %14s %8s\n", "Type", "Name", "Count", "Share");
        for (const char t : types) {
            const std::uint64_t n = stats.counts[static_cast<std::uint8_t>(t)];
            std::printf("%-4c %-32.*s %14llu %7.2f%%\n", t,
                        static_cast<int>(itch::message_type_name(t).size()),
                        itch::message_type_name(t).data(), static_cast<unsigned long long>(n),
                        100.0 * static_cast<double>(n) / static_cast<double>(result.messages));
        }
        std::printf("%-37s %14llu\n\n", "Total", static_cast<unsigned long long>(result.messages));

        // ---- Sanity checks -----------------------------------------------------
        std::printf("Timestamps: ");
        print_time(stats.first_timestamp);
        std::printf(" -> ");
        print_time(stats.last_timestamp);
        std::printf(", %llu backward steps%s\n",
                    static_cast<unsigned long long>(stats.backward_steps),
                    stats.backward_steps == 0 ? " (monotonic)" : "");

        std::printf("System events:\n");
        for (const auto& e : stats.system_events) {
            std::printf("  %c  ", e.event_code);
            print_time(e.timestamp_ns);
            std::printf("\n");
        }
        const bool day_bounded =
            !stats.system_events.empty() &&
            stats.system_events.front().event_code == itch::event_code::kStartOfMessages &&
            stats.system_events.back().event_code == itch::event_code::kEndOfMessages;
        std::printf("Day bounded by start/end of messages ('O' ... 'C'): %s\n\n",
                    day_bounded ? "yes" : "NO");

        // ---- Throughput --------------------------------------------------------
        // Pass 1 (above): framing, per-type counts, timestamp check. Runs first, so it
        // includes page faults when the file is not already in the page cache.
        // Pass 2: full field decoding of every book and trade message.
        DecodeAllHandler decoder;
        const auto decode_start = std::chrono::steady_clock::now();
        const itch::ParseResult decode_result = itch::parse(file.bytes(), decoder);
        const auto decode_stop = std::chrono::steady_clock::now();
        const double decode_seconds =
            std::chrono::duration<double>(decode_stop - decode_start).count();

        const auto report = [&](const char* label, double secs) {
            std::printf("%-22s %.3f s  |  %6.1f M msg/s  |  %5.0f MB/s  |  %5.1f ns/msg\n", label,
                        secs, static_cast<double>(result.messages) / secs / 1e6,
                        static_cast<double>(result.bytes_consumed) / secs / 1e6,
                        secs * 1e9 / static_cast<double>(result.messages));
        };
        report("Pass 1 (scan + count):", seconds);
        report("Pass 2 (full decode):", decode_seconds);
        std::printf("Decode checksum: %016llx\n",
                    static_cast<unsigned long long>(decoder.checksum));
        if (decode_result.messages != result.messages) {
            std::fprintf(stderr, "decode pass disagreed on message count\n");
            return 1;
        }

        if (!result.ok()) {
            std::fprintf(stderr, "parse error at byte %zu: %.*s\n", result.bytes_consumed,
                         static_cast<int>(itch::to_string(result.error).size()),
                         itch::to_string(result.error).data());
            return 1;
        }
        const bool all_checks_pass = day_bounded && stats.backward_steps == 0;
        return all_checks_pass ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
