// lob::Replayer on testdata/sample.itch. The expected values are listed in testdata/README.md;
// python/tests/test_replayer.py asserts the same values through the Python bindings.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "lob/replayer.hpp"

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
const std::string kSample = std::string(LOB_TESTDATA_DIR) + "/sample.itch";

// Level `level` (0 = best) of row `row` in a [rows x depth] array.
std::int64_t at(const std::vector<std::int64_t>& v, const lob::SnapshotData& d, std::size_t row,
                std::size_t level) {
    return v[row * d.depth + level];
}

}  // namespace

TEST(Replayer, ReadsDirectoryAndTimeRange) {
    const lob::Replayer r(kSample, {});
    EXPECT_EQ(r.symbols(), (std::vector<std::string>{"AAA", "BBB"}));
    EXPECT_EQ(r.first_timestamp(), 1U * kSecond);
    EXPECT_EQ(r.last_timestamp(), 15U * kSecond);
}

TEST(Replayer, RejectsUnknownOrDuplicateSymbols) {
    EXPECT_THROW(lob::Replayer(kSample, {"ZZZ"}), std::invalid_argument);
    EXPECT_THROW(lob::Replayer(kSample, {"AAA", "AAA"}), std::invalid_argument);
    EXPECT_THROW(lob::Replayer("/nonexistent/file.itch", {}), std::system_error);
}

TEST(Replayer, RejectsBadSnapshotArguments) {
    const lob::Replayer r(kSample, {"AAA"});
    EXPECT_THROW((void)r.snapshots(0, 5), std::invalid_argument);
    EXPECT_THROW((void)r.snapshots(kSecond, 0), std::invalid_argument);
    EXPECT_THROW((void)r.snapshots(kSecond, 5, 10 * kSecond, 9 * kSecond), std::invalid_argument);
    // 1 ns samples over 24 hours: far beyond the 4 GiB output limit, rejected up front.
    EXPECT_THROW((void)r.snapshots(1, 5, 0, 24ULL * 3600 * kSecond), std::invalid_argument);
}

TEST(Replayer, SnapshotsMatchHandComputedBooks) {
    const lob::Replayer r(kSample, {"AAA", "BBB"});
    const lob::SnapshotData d = r.snapshots(kSecond, 2);

    // Default grid: 1 s (first message) to 15 s (last), every second, two symbols per sample.
    ASSERT_EQ(d.rows, 15U * 2);
    ASSERT_EQ(d.depth, 2U);
    const auto row = [](int second, int symbol) {
        return static_cast<std::size_t>((second - 1) * 2 + symbol);
    };
    EXPECT_EQ(d.timestamp[row(10, 0)], 10 * kSecond);
    EXPECT_EQ(d.symbol[row(10, 1)], 1);

    // Before any orders: everything empty.
    EXPECT_EQ(at(d.bid_price, d, row(9, 0), 0), 0);
    EXPECT_EQ(at(d.ask_size, d, row(9, 1), 0), 0);

    // 10 s: the add at exactly 10.0 s is included, the later ones are not.
    EXPECT_EQ(at(d.bid_price, d, row(10, 0), 0), 100'000);
    EXPECT_EQ(at(d.bid_size, d, row(10, 0), 0), 100);
    EXPECT_EQ(at(d.bid_price, d, row(10, 0), 1), 0);
    EXPECT_EQ(at(d.ask_price, d, row(10, 0), 0), 0);

    // 11 s: full AAA book after the 50-share execution at exactly 11.0 s; BBB both sides.
    EXPECT_EQ(at(d.bid_price, d, row(11, 0), 0), 100'000);
    EXPECT_EQ(at(d.bid_price, d, row(11, 0), 1), 99'900);
    EXPECT_EQ(at(d.bid_size, d, row(11, 0), 1), 200);
    EXPECT_EQ(at(d.ask_price, d, row(11, 0), 0), 100'100);
    EXPECT_EQ(at(d.ask_size, d, row(11, 0), 0), 100);
    EXPECT_EQ(at(d.ask_price, d, row(11, 0), 1), 100'200);
    EXPECT_EQ(at(d.ask_size, d, row(11, 0), 1), 50);
    EXPECT_EQ(at(d.bid_price, d, row(11, 1), 0), 499'500);
    EXPECT_EQ(at(d.ask_price, d, row(11, 1), 0), 500'000);
    EXPECT_EQ(at(d.ask_size, d, row(11, 1), 0), 300);

    // 12 s: partial cancel of the $9.99 bid, and the $10.00 bid fully executed.
    EXPECT_EQ(at(d.bid_price, d, row(12, 0), 0), 99'900);
    EXPECT_EQ(at(d.bid_size, d, row(12, 0), 0), 150);
    EXPECT_EQ(at(d.bid_price, d, row(12, 0), 1), 0);

    // 13 s onwards: replace moved $10.02 x 50 to $10.015 x 80; the BBB bid was deleted.
    for (int t = 13; t <= 15; ++t) {
        EXPECT_EQ(at(d.ask_price, d, row(t, 0), 1), 100'150) << t;
        EXPECT_EQ(at(d.ask_size, d, row(t, 0), 1), 80) << t;
        EXPECT_EQ(at(d.bid_price, d, row(t, 1), 0), 0) << t;
        EXPECT_EQ(at(d.ask_price, d, row(t, 1), 0), 500'000) << t;
    }
}

TEST(Replayer, SnapshotWindowAndSymbolSubset) {
    const lob::Replayer r(kSample, {"BBB"});
    const lob::SnapshotData d = r.snapshots(kSecond / 2, 1, 11 * kSecond, 13 * kSecond);
    ASSERT_EQ(d.rows, 5U);  // 11.0, 11.5, 12.0, 12.5, 13.0
    EXPECT_EQ(d.timestamp.front(), 11 * kSecond);
    EXPECT_EQ(d.timestamp.back(), 13 * kSecond);
    EXPECT_EQ(d.bid_price[3], 499'500);  // 12.5 s
    EXPECT_EQ(d.bid_price[4], 0);        // 13.0 s: deleted at exactly 13.0
}

TEST(Replayer, TradesMatchHandComputedExecutions) {
    const lob::Replayer r(kSample, {"AAA", "BBB"});
    const lob::TradeData t = r.trades();

    ASSERT_EQ(t.timestamp.size(), 4U);
    EXPECT_EQ(t.timestamp, (std::vector<std::int64_t>{11 * kSecond, 12 * kSecond,
                                                      13 * kSecond + kSecond / 2, 14 * kSecond}));
    EXPECT_EQ(t.symbol, (std::vector<std::int32_t>{0, 0, 1, 0}));
    EXPECT_EQ(t.price, (std::vector<std::int64_t>{100'100, 99'950, 499'800, 100'000}));
    EXPECT_EQ(t.shares, (std::vector<std::int64_t>{50, 100, 25, 1'000}));
    EXPECT_EQ(t.side, (std::vector<std::int8_t>{1, -1, 1, 0}));
    EXPECT_EQ(t.match_number, (std::vector<std::uint64_t>{1001, 1002, 1003, 1004}));
    EXPECT_EQ(t.kind, (std::vector<std::uint8_t>{'E', 'C', 'P', 'Q'}));
    EXPECT_EQ(t.printable, (std::vector<std::uint8_t>{1, 1, 1, 1}));
}

TEST(Replayer, TradesOnlyForChosenSymbols) {
    const lob::Replayer r(kSample, {"BBB"});
    const lob::TradeData t = r.trades();
    ASSERT_EQ(t.timestamp.size(), 1U);
    EXPECT_EQ(t.kind[0], 'P');
    EXPECT_EQ(t.symbol[0], 0);  // index into this Replayer's symbols
}
