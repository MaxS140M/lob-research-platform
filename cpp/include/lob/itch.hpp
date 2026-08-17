#pragma once

// NASDAQ TotalView-ITCH 5.0 message definitions and decoders.
//
// Wire format: every field is big-endian, prices are unsigned integers with 4 implied
// decimals (1823500 == $182.3500), and timestamps are 6-byte nanoseconds since midnight.
// Byte offsets below are from the start of the message, i.e. offset 0 is the type byte.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace lob::itch {

// ---- Big-endian field readers ---------------------------------------------

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | std::uint16_t{p[1]});
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}

inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{read_be16(p)} << 32) | std::uint64_t{read_be32(p + 2)};
}

inline std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t{read_be32(p)} << 32) | std::uint64_t{read_be32(p + 4)};
}

// ---- Field types ----------------------------------------------------------

// Price with 4 implied decimal places. Never converted to floating point in the engine.
using Price = std::uint32_t;
inline constexpr Price kPriceScale = 10'000;

// Stock symbol: 8 ASCII characters, right-padded with spaces.
using Symbol = std::array<char, 8>;

inline Symbol read_symbol(const std::uint8_t* p) noexcept {
    Symbol s;
    std::memcpy(s.data(), p, s.size());
    return s;
}

// The symbol without its trailing space padding.
inline std::string_view symbol_view(const Symbol& s) noexcept {
    std::size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s.data(), n};
}

// ---- Messages -------------------------------------------------------------

// Fields shared by every ITCH 5.0 message (offsets 1-10).
struct MessageHeader {
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    std::uint64_t timestamp_ns;  // nanoseconds since midnight
};

// 'S': market-wide session events.
struct SystemEvent : MessageHeader {
    char event_code;  // 'O' start of messages ... 'C' end of messages
};

namespace event_code {
inline constexpr char kStartOfMessages = 'O';
inline constexpr char kStartOfSystemHours = 'S';
inline constexpr char kStartOfMarketHours = 'Q';
inline constexpr char kEndOfMarketHours = 'M';
inline constexpr char kEndOfSystemHours = 'E';
inline constexpr char kEndOfMessages = 'C';
}  // namespace event_code

// 'R': maps a stock locate code to a symbol at the start of the day.
struct StockDirectory : MessageHeader {
    Symbol stock;
    char market_category;
    char financial_status_indicator;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    std::array<char, 2> issue_sub_type;
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    std::uint32_t etp_leverage_factor;
    char inverse_indicator;
};

// 'A': a new visible order on the book.
struct AddOrder : MessageHeader {
    std::uint64_t order_ref;
    char side;  // 'B' buy or 'S' sell
    std::uint32_t shares;
    Symbol stock;
    Price price;
};

// 'F': an add order with market participant attribution.
struct AddOrderMpid : AddOrder {
    std::array<char, 4> attribution;
};

// 'E': part or all of a resting order executed at its limit price.
struct OrderExecuted : MessageHeader {
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

// 'C': part or all of a resting order executed at a price other than its limit.
struct OrderExecutedWithPrice : MessageHeader {
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char printable;  // 'Y' or 'N': whether it should count toward volume
    Price execution_price;
};

// 'X': a partial cancellation; the order stays on the book with fewer shares.
struct OrderCancel : MessageHeader {
    std::uint64_t order_ref;
    std::uint32_t cancelled_shares;
};

// 'D': the order is removed from the book entirely.
struct OrderDelete : MessageHeader {
    std::uint64_t order_ref;
};

// 'U': cancel-replace. The original order is removed and a new one added on the same side.
struct OrderReplace : MessageHeader {
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    Price price;
};

// 'P': an execution against a non-displayed order. Does not affect the visible book.
struct Trade : MessageHeader {
    std::uint64_t order_ref;  // always 0 in practice
    char side;
    std::uint32_t shares;
    Symbol stock;
    Price price;
    std::uint64_t match_number;
};

// 'Q': the bulk print of an opening, closing, halt or IPO cross.
struct CrossTrade : MessageHeader {
    std::uint64_t shares;  // 8 bytes on the wire, unlike every other share count
    Symbol stock;
    Price cross_price;
    std::uint64_t match_number;
    char cross_type;
};

// 'B': a previously reported execution was broken.
struct BrokenTrade : MessageHeader {
    std::uint64_t match_number;
};

// ---- Message sizes --------------------------------------------------------

// Payload length in bytes (excluding the 2-byte length prefix) of each ITCH 5.0 message
// type, or 0 for a type this spec version does not define.
constexpr std::size_t message_size(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

constexpr std::string_view message_type_name(char type) noexcept {
    switch (type) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order";
        case 'F': return "Add Order (MPID)";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed With Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (Non-Cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'N': return "Retail Price Improvement";
        case 'O': return "Direct Listing Price Discovery";
        default: return "Unknown";
    }
}

// ---- Decoders -------------------------------------------------------------
// Each takes a pointer to the type byte of a message whose length has already been
// validated against message_size().

inline MessageHeader decode_header(const std::uint8_t* p) noexcept {
    return {read_be16(p + 1), read_be16(p + 3), read_be48(p + 5)};
}

inline SystemEvent decode_system_event(const std::uint8_t* p) noexcept {
    SystemEvent m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.event_code = static_cast<char>(p[11]);
    return m;
}

inline StockDirectory decode_stock_directory(const std::uint8_t* p) noexcept {
    StockDirectory m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.stock = read_symbol(p + 11);
    m.market_category = static_cast<char>(p[19]);
    m.financial_status_indicator = static_cast<char>(p[20]);
    m.round_lot_size = read_be32(p + 21);
    m.round_lots_only = static_cast<char>(p[25]);
    m.issue_classification = static_cast<char>(p[26]);
    m.issue_sub_type = {static_cast<char>(p[27]), static_cast<char>(p[28])};
    m.authenticity = static_cast<char>(p[29]);
    m.short_sale_threshold_indicator = static_cast<char>(p[30]);
    m.ipo_flag = static_cast<char>(p[31]);
    m.luld_reference_price_tier = static_cast<char>(p[32]);
    m.etp_flag = static_cast<char>(p[33]);
    m.etp_leverage_factor = read_be32(p + 34);
    m.inverse_indicator = static_cast<char>(p[38]);
    return m;
}

inline AddOrder decode_add_order(const std::uint8_t* p) noexcept {
    AddOrder m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    m.side = static_cast<char>(p[19]);
    m.shares = read_be32(p + 20);
    m.stock = read_symbol(p + 24);
    m.price = read_be32(p + 32);
    return m;
}

inline AddOrderMpid decode_add_order_mpid(const std::uint8_t* p) noexcept {
    AddOrderMpid m{};
    static_cast<AddOrder&>(m) = decode_add_order(p);
    std::memcpy(m.attribution.data(), p + 36, m.attribution.size());
    return m;
}

inline OrderExecuted decode_order_executed(const std::uint8_t* p) noexcept {
    OrderExecuted m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    m.executed_shares = read_be32(p + 19);
    m.match_number = read_be64(p + 23);
    return m;
}

inline OrderExecutedWithPrice decode_order_executed_with_price(const std::uint8_t* p) noexcept {
    OrderExecutedWithPrice m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    m.executed_shares = read_be32(p + 19);
    m.match_number = read_be64(p + 23);
    m.printable = static_cast<char>(p[31]);
    m.execution_price = read_be32(p + 32);
    return m;
}

inline OrderCancel decode_order_cancel(const std::uint8_t* p) noexcept {
    OrderCancel m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    m.cancelled_shares = read_be32(p + 19);
    return m;
}

inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
    OrderDelete m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    return m;
}

inline OrderReplace decode_order_replace(const std::uint8_t* p) noexcept {
    OrderReplace m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.original_order_ref = read_be64(p + 11);
    m.new_order_ref = read_be64(p + 19);
    m.shares = read_be32(p + 27);
    m.price = read_be32(p + 31);
    return m;
}

inline Trade decode_trade(const std::uint8_t* p) noexcept {
    Trade m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.order_ref = read_be64(p + 11);
    m.side = static_cast<char>(p[19]);
    m.shares = read_be32(p + 20);
    m.stock = read_symbol(p + 24);
    m.price = read_be32(p + 32);
    m.match_number = read_be64(p + 36);
    return m;
}

inline CrossTrade decode_cross_trade(const std::uint8_t* p) noexcept {
    CrossTrade m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.shares = read_be64(p + 11);
    m.stock = read_symbol(p + 19);
    m.cross_price = read_be32(p + 27);
    m.match_number = read_be64(p + 31);
    m.cross_type = static_cast<char>(p[39]);
    return m;
}

inline BrokenTrade decode_broken_trade(const std::uint8_t* p) noexcept {
    BrokenTrade m{};
    static_cast<MessageHeader&>(m) = decode_header(p);
    m.match_number = read_be64(p + 11);
    return m;
}

}  // namespace lob::itch
