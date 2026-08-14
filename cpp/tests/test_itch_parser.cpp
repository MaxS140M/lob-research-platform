#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "lob/itch.hpp"
#include "lob/itch_parser.hpp"

using namespace lob::itch;

namespace {

// Builds a raw ITCH message byte by byte, big-endian, exactly as it appears on the wire.
class Bytes {
public:
    Bytes& u8(std::uint8_t v) {
        b_.push_back(v);
        return *this;
    }
    Bytes& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    Bytes& be16(std::uint16_t v) {
        return u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v));
    }
    Bytes& be32(std::uint32_t v) {
        return be16(static_cast<std::uint16_t>(v >> 16)).be16(static_cast<std::uint16_t>(v));
    }
    Bytes& be48(std::uint64_t v) {
        return be16(static_cast<std::uint16_t>(v >> 32)).be32(static_cast<std::uint32_t>(v));
    }
    Bytes& be64(std::uint64_t v) {
        return be32(static_cast<std::uint32_t>(v >> 32)).be32(static_cast<std::uint32_t>(v));
    }
    // Fixed-width alpha field, right-padded with spaces.
    Bytes& alpha(std::string_view s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) ch(i < s.size() ? s[i] : ' ');
        return *this;
    }
    Bytes& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return ch(type).be16(locate).be16(tracking).be48(ts);
    }

    [[nodiscard]] const std::uint8_t* data() const { return b_.data(); }
    [[nodiscard]] std::size_t size() const { return b_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& vec() const { return b_; }

private:
    std::vector<std::uint8_t> b_;
};

// Concatenates messages into a stream, each with its 2-byte length prefix.
std::vector<std::uint8_t> frame(std::initializer_list<Bytes> messages) {
    std::vector<std::uint8_t> out;
    for (const auto& m : messages) {
        out.push_back(static_cast<std::uint8_t>(m.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(m.size()));
        out.insert(out.end(), m.vec().begin(), m.vec().end());
    }
    return out;
}

// Values chosen so every byte of each field is distinct and non-zero.
constexpr std::uint16_t kLocate = 0x0102;
constexpr std::uint16_t kTracking = 0x0304;
constexpr std::uint64_t kTimestamp = 0x0A0B0C0D0E0FULL;  // ~3.05 hours after midnight
constexpr std::uint64_t kOrderRef = 0x1112131415161718ULL;
constexpr std::uint64_t kMatch = 0x2122232425262728ULL;

void expect_header(const MessageHeader& h) {
    EXPECT_EQ(h.stock_locate, kLocate);
    EXPECT_EQ(h.tracking_number, kTracking);
    EXPECT_EQ(h.timestamp_ns, kTimestamp);
}

Bytes add_order_bytes(char type = 'A') {
    Bytes b;
    b.header(type, kLocate, kTracking, kTimestamp)
        .be64(kOrderRef)
        .ch('B')
        .be32(100)
        .alpha("AAPL", 8)
        .be32(1'823'500);
    return b;
}

}  // namespace

// ---- Byte readers ---------------------------------------------------------

TEST(ItchBytes, ReadsBigEndianIntegers) {
    const std::uint8_t b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(read_be16(b), 0x0102U);
    EXPECT_EQ(read_be32(b), 0x01020304U);
    EXPECT_EQ(read_be48(b), 0x010203040506ULL);
    EXPECT_EQ(read_be64(b), 0x0102030405060708ULL);
}

TEST(ItchBytes, ReadsHighBitBytesWithoutSignExtension) {
    const std::uint8_t b[] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};
    EXPECT_EQ(read_be16(b), 0xFFFEU);
    EXPECT_EQ(read_be32(b), 0xFFFEFDFCU);
    EXPECT_EQ(read_be48(b), 0xFFFEFDFCFBFAULL);
    EXPECT_EQ(read_be64(b), 0xFFFEFDFCFBFAF9F8ULL);
}

TEST(ItchBytes, SymbolViewTrimsPadding) {
    EXPECT_EQ(symbol_view(Symbol{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '}), "AAPL");
    EXPECT_EQ(symbol_view(Symbol{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}), "ABCDEFGH");
    EXPECT_EQ(symbol_view(Symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}), "");
}

// ---- Decoders: one test per message type ----------------------------------

TEST(ItchDecode, SystemEvent) {
    Bytes b;
    b.header('S', 0, kTracking, kTimestamp).ch('O');
    ASSERT_EQ(b.size(), message_size('S'));

    const auto m = decode_system_event(b.data());
    EXPECT_EQ(m.stock_locate, 0U);
    EXPECT_EQ(m.tracking_number, kTracking);
    EXPECT_EQ(m.timestamp_ns, kTimestamp);
    EXPECT_EQ(m.event_code, event_code::kStartOfMessages);
}

TEST(ItchDecode, StockDirectory) {
    Bytes b;
    b.header('R', kLocate, kTracking, kTimestamp)
        .alpha("MSFT", 8)
        .ch('Q')        // market category
        .ch('N')        // financial status
        .be32(100)      // round lot size
        .ch('N')        // round lots only
        .ch('C')        // issue classification
        .alpha("Z", 2)  // issue sub-type
        .ch('P')        // authenticity
        .ch('N')        // short sale threshold
        .ch(' ')        // IPO flag
        .ch('1')        // LULD tier
        .ch('N')        // ETP flag
        .be32(0x31323334)
        .ch('N');
    ASSERT_EQ(b.size(), message_size('R'));

    const auto m = decode_stock_directory(b.data());
    expect_header(m);
    EXPECT_EQ(symbol_view(m.stock), "MSFT");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.financial_status_indicator, 'N');
    EXPECT_EQ(m.round_lot_size, 100U);
    EXPECT_EQ(m.round_lots_only, 'N');
    EXPECT_EQ(m.issue_classification, 'C');
    EXPECT_EQ(m.issue_sub_type[0], 'Z');
    EXPECT_EQ(m.issue_sub_type[1], ' ');
    EXPECT_EQ(m.authenticity, 'P');
    EXPECT_EQ(m.short_sale_threshold_indicator, 'N');
    EXPECT_EQ(m.ipo_flag, ' ');
    EXPECT_EQ(m.luld_reference_price_tier, '1');
    EXPECT_EQ(m.etp_flag, 'N');
    EXPECT_EQ(m.etp_leverage_factor, 0x31323334U);
    EXPECT_EQ(m.inverse_indicator, 'N');
}

TEST(ItchDecode, AddOrder) {
    const Bytes b = add_order_bytes();
    ASSERT_EQ(b.size(), message_size('A'));

    const auto m = decode_add_order(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100U);
    EXPECT_EQ(symbol_view(m.stock), "AAPL");
    EXPECT_EQ(m.price, 1'823'500U);  // $182.35
}

TEST(ItchDecode, AddOrderMpid) {
    Bytes b = add_order_bytes('F');
    b.alpha("GSCO", 4);
    ASSERT_EQ(b.size(), message_size('F'));

    const auto m = decode_add_order_mpid(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100U);
    EXPECT_EQ(symbol_view(m.stock), "AAPL");
    EXPECT_EQ(m.price, 1'823'500U);
    EXPECT_EQ(std::string_view(m.attribution.data(), m.attribution.size()), "GSCO");
}

TEST(ItchDecode, OrderExecuted) {
    Bytes b;
    b.header('E', kLocate, kTracking, kTimestamp).be64(kOrderRef).be32(0x41424344).be64(kMatch);
    ASSERT_EQ(b.size(), message_size('E'));

    const auto m = decode_order_executed(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
    EXPECT_EQ(m.executed_shares, 0x41424344U);
    EXPECT_EQ(m.match_number, kMatch);
}

TEST(ItchDecode, OrderExecutedWithPrice) {
    Bytes b;
    b.header('C', kLocate, kTracking, kTimestamp)
        .be64(kOrderRef)
        .be32(50)
        .be64(kMatch)
        .ch('Y')
        .be32(1'823'400);
    ASSERT_EQ(b.size(), message_size('C'));

    const auto m = decode_order_executed_with_price(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
    EXPECT_EQ(m.executed_shares, 50U);
    EXPECT_EQ(m.match_number, kMatch);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.execution_price, 1'823'400U);
}

TEST(ItchDecode, OrderCancel) {
    Bytes b;
    b.header('X', kLocate, kTracking, kTimestamp).be64(kOrderRef).be32(25);
    ASSERT_EQ(b.size(), message_size('X'));

    const auto m = decode_order_cancel(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
    EXPECT_EQ(m.cancelled_shares, 25U);
}

TEST(ItchDecode, OrderDelete) {
    Bytes b;
    b.header('D', kLocate, kTracking, kTimestamp).be64(kOrderRef);
    ASSERT_EQ(b.size(), message_size('D'));

    const auto m = decode_order_delete(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, kOrderRef);
}

TEST(ItchDecode, OrderReplace) {
    Bytes b;
    b.header('U', kLocate, kTracking, kTimestamp)
        .be64(kOrderRef)
        .be64(kOrderRef + 1)
        .be32(300)
        .be32(1'823'600);
    ASSERT_EQ(b.size(), message_size('U'));

    const auto m = decode_order_replace(b.data());
    expect_header(m);
    EXPECT_EQ(m.original_order_ref, kOrderRef);
    EXPECT_EQ(m.new_order_ref, kOrderRef + 1);
    EXPECT_EQ(m.shares, 300U);
    EXPECT_EQ(m.price, 1'823'600U);
}

TEST(ItchDecode, Trade) {
    Bytes b;
    b.header('P', kLocate, kTracking, kTimestamp)
        .be64(0)
        .ch('S')
        .be32(200)
        .alpha("TSLA", 8)
        .be32(2'500'000)
        .be64(kMatch);
    ASSERT_EQ(b.size(), message_size('P'));

    const auto m = decode_trade(b.data());
    expect_header(m);
    EXPECT_EQ(m.order_ref, 0U);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.shares, 200U);
    EXPECT_EQ(symbol_view(m.stock), "TSLA");
    EXPECT_EQ(m.price, 2'500'000U);
    EXPECT_EQ(m.match_number, kMatch);
}

TEST(ItchDecode, CrossTradeHasEightByteShares) {
    constexpr std::uint64_t kShares = 0x0000000500000001ULL;  // > 2^32
    Bytes b;
    b.header('Q', kLocate, kTracking, kTimestamp)
        .be64(kShares)
        .alpha("SPY", 8)
        .be32(2'990'000)
        .be64(kMatch)
        .ch('O');
    ASSERT_EQ(b.size(), message_size('Q'));

    const auto m = decode_cross_trade(b.data());
    expect_header(m);
    EXPECT_EQ(m.shares, kShares);
    EXPECT_EQ(symbol_view(m.stock), "SPY");
    EXPECT_EQ(m.cross_price, 2'990'000U);
    EXPECT_EQ(m.match_number, kMatch);
    EXPECT_EQ(m.cross_type, 'O');
}

TEST(ItchDecode, BrokenTrade) {
    Bytes b;
    b.header('B', kLocate, kTracking, kTimestamp).be64(kMatch);
    ASSERT_EQ(b.size(), message_size('B'));

    const auto m = decode_broken_trade(b.data());
    expect_header(m);
    EXPECT_EQ(m.match_number, kMatch);
}

// ---- Stream parser --------------------------------------------------------

namespace {

struct Recorder {
    std::vector<char> types;  // from on_message, every message in order
    std::vector<SystemEvent> system_events;
    std::vector<AddOrder> adds;
    std::vector<AddOrderMpid> mpid_adds;
    std::vector<OrderDelete> deletes;

    void on_message(char type, std::span<const std::uint8_t>) { types.push_back(type); }
    void on_system_event(const SystemEvent& m) { system_events.push_back(m); }
    void on_add_order(const AddOrder& m) { adds.push_back(m); }
    void on_add_order_mpid(const AddOrderMpid& m) { mpid_adds.push_back(m); }
    void on_order_delete(const OrderDelete& m) { deletes.push_back(m); }
};

// Implements only on_add_order, so 'F' messages should fall back to it.
struct AddOnly {
    std::vector<AddOrder> adds;
    void on_add_order(const AddOrder& m) { adds.push_back(m); }
};

Bytes system_event_bytes(char code) {
    Bytes b;
    b.header('S', 0, 0, kTimestamp).ch(code);
    return b;
}

Bytes delete_bytes() {
    Bytes b;
    b.header('D', kLocate, kTracking, kTimestamp).be64(kOrderRef);
    return b;
}

}  // namespace

TEST(ItchParser, EmptyBufferParsesNothing) {
    Recorder r;
    const auto result = parse({}, r);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.messages, 0U);
    EXPECT_EQ(result.bytes_consumed, 0U);
}

TEST(ItchParser, DispatchesEachMessageToItsCallbackInOrder) {
    Bytes mpid = add_order_bytes('F');
    mpid.alpha("GSCO", 4);
    const auto stream = frame({system_event_bytes('O'), add_order_bytes(), mpid, delete_bytes(),
                               system_event_bytes('C')});

    Recorder r;
    const auto result = parse(stream, r);

    ASSERT_TRUE(result.ok()) << to_string(result.error);
    EXPECT_EQ(result.messages, 5U);
    EXPECT_EQ(result.bytes_consumed, stream.size());
    EXPECT_EQ(r.types, (std::vector<char>{'S', 'A', 'F', 'D', 'S'}));
    ASSERT_EQ(r.system_events.size(), 2U);
    EXPECT_EQ(r.system_events.front().event_code, event_code::kStartOfMessages);
    EXPECT_EQ(r.system_events.back().event_code, event_code::kEndOfMessages);
    ASSERT_EQ(r.adds.size(), 1U);  // 'F' goes to on_add_order_mpid, not here
    EXPECT_EQ(r.adds[0].price, 1'823'500U);
    ASSERT_EQ(r.mpid_adds.size(), 1U);
    EXPECT_EQ(r.mpid_adds[0].order_ref, kOrderRef);
    ASSERT_EQ(r.deletes.size(), 1U);
    EXPECT_EQ(r.deletes[0].order_ref, kOrderRef);
}

TEST(ItchParser, MpidAddFallsBackToOnAddOrder) {
    Bytes mpid = add_order_bytes('F');
    mpid.alpha("GSCO", 4);
    const auto stream = frame({add_order_bytes(), mpid});

    AddOnly h;
    const auto result = parse(stream, h);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(h.adds.size(), 2U);
    EXPECT_EQ(h.adds[1].order_ref, kOrderRef);
    EXPECT_EQ(h.adds[1].shares, 100U);
}

TEST(ItchParser, HandlerWithNoCallbacksStillCountsMessages) {
    struct Nothing {};
    const auto stream = frame({add_order_bytes(), delete_bytes()});
    Nothing h;
    const auto result = parse(stream, h);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.messages, 2U);
}

TEST(ItchParser, UnknownTypeIsSeenByOnMessageOnly) {
    Bytes unknown;
    unknown.ch('z').alpha("payload", 7);
    const auto stream = frame({unknown, delete_bytes()});

    Recorder r;
    const auto result = parse(stream, r);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(r.types, (std::vector<char>{'z', 'D'}));
    EXPECT_EQ(r.deletes.size(), 1U);
}

TEST(ItchParser, StopsAtTruncatedLengthPrefix) {
    auto stream = frame({delete_bytes()});
    const std::size_t first = stream.size();
    stream.push_back(0x00);  // half a length prefix

    Recorder r;
    const auto result = parse(stream, r);

    EXPECT_EQ(result.error, ParseError::kTruncatedLength);
    EXPECT_EQ(result.messages, 1U);
    EXPECT_EQ(result.bytes_consumed, first);
}

TEST(ItchParser, StopsAtTruncatedMessage) {
    auto stream = frame({delete_bytes(), delete_bytes()});
    stream.pop_back();  // cut the last byte of the second message

    Recorder r;
    const auto result = parse(stream, r);

    EXPECT_EQ(result.error, ParseError::kTruncatedMessage);
    EXPECT_EQ(result.messages, 1U);
    EXPECT_EQ(r.deletes.size(), 1U);
}

TEST(ItchParser, RejectsLengthThatDoesNotMatchType) {
    Bytes short_add;
    short_add.header('A', kLocate, kTracking, kTimestamp).be64(kOrderRef);  // 19 bytes, not 36
    const auto stream = frame({delete_bytes(), short_add});

    Recorder r;
    const auto result = parse(stream, r);

    EXPECT_EQ(result.error, ParseError::kBadLength);
    EXPECT_EQ(result.messages, 1U);
    EXPECT_TRUE(r.adds.empty());  // never decoded past the end of the message
}

TEST(ItchParser, RejectsZeroLength) {
    const std::vector<std::uint8_t> stream = {0x00, 0x00};
    Recorder r;
    EXPECT_EQ(parse(stream, r).error, ParseError::kBadLength);
}

// ---- Size table vs. the spec ----------------------------------------------

TEST(ItchSizes, MatchSpecification) {
    // ITCH 5.0 spec, section 1: total message length per type.
    const std::pair<char, std::size_t> expected[] = {
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48},
    };
    for (const auto& [type, size] : expected) {
        EXPECT_EQ(message_size(type), size) << "type " << type;
    }
    EXPECT_EQ(message_size('z'), 0U);
}
