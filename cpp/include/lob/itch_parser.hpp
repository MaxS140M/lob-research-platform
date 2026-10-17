#pragma once

// Zero-copy ITCH 5.0 stream parser.
//
// parse() walks a buffer of length-prefixed messages and calls the matching callback on a
// user-supplied handler. The handler is a template parameter, so dispatch is resolved at
// compile time and inlined: no virtual calls and no allocation per message. A handler
// implements only the callbacks it cares about; missing ones are skipped at compile time.
//
//   struct Handler {
//       void on_message(char type, std::span<const std::uint8_t> payload);  // every message
//       void on_system_event(const SystemEvent&);
//       void on_stock_directory(const StockDirectory&);
//       void on_add_order(const AddOrder&);          // also receives 'F' if the handler
//       void on_add_order_mpid(const AddOrderMpid&); // has no on_add_order_mpid
//       void on_order_executed(const OrderExecuted&);
//       void on_order_executed_with_price(const OrderExecutedWithPrice&);
//       void on_order_cancel(const OrderCancel&);
//       void on_order_delete(const OrderDelete&);
//       void on_order_replace(const OrderReplace&);
//       void on_trade(const Trade&);
//       void on_cross_trade(const CrossTrade&);
//       void on_broken_trade(const BrokenTrade&);
//   };

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "lob/itch.hpp"

namespace lob::itch {

enum class ParseError {
    kNone,
    kTruncatedLength,   // fewer than 2 bytes left where a length prefix was expected
    kTruncatedMessage,  // the length prefix runs past the end of the buffer
    kBadLength,         // zero length, or a length that does not match the message type
};

constexpr std::string_view to_string(ParseError e) noexcept {
    switch (e) {
        case ParseError::kNone: return "none";
        case ParseError::kTruncatedLength: return "truncated length prefix";
        case ParseError::kTruncatedMessage: return "truncated message";
        case ParseError::kBadLength: return "bad message length";
    }
    return "unknown";
}

struct ParseResult {
    std::uint64_t messages = 0;      // messages successfully delivered
    std::size_t bytes_consumed = 0;  // offset of the first byte not parsed
    ParseError error = ParseError::kNone;

    [[nodiscard]] bool ok() const noexcept { return error == ParseError::kNone; }
};

namespace detail {

// Delivers one validated message to the handler. Returns false if the length is wrong.
template <class Handler>
bool dispatch(const std::uint8_t* m, std::size_t len, Handler& h) {
    const char type = static_cast<char>(m[0]);
    const std::size_t expected = message_size(type);
    if (expected != 0 && len != expected) return false;

    if constexpr (requires(Handler& hh, std::span<const std::uint8_t> s) {
                      hh.on_message(type, s);
                  }) {
        h.on_message(type, std::span<const std::uint8_t>{m, len});
    }

    switch (type) {
        case 'S':
            if constexpr (requires(Handler& hh, const SystemEvent& e) { hh.on_system_event(e); })
                h.on_system_event(decode_system_event(m));
            break;
        case 'R':
            if constexpr (requires(Handler& hh, const StockDirectory& e) {
                              hh.on_stock_directory(e);
                          })
                h.on_stock_directory(decode_stock_directory(m));
            break;
        case 'A':
            if constexpr (requires(Handler& hh, const AddOrder& e) { hh.on_add_order(e); })
                h.on_add_order(decode_add_order(m));
            break;
        case 'F':
            if constexpr (requires(Handler& hh, const AddOrderMpid& e) { hh.on_add_order_mpid(e); })
                h.on_add_order_mpid(decode_add_order_mpid(m));
            else if constexpr (requires(Handler& hh, const AddOrder& e) { hh.on_add_order(e); })
                h.on_add_order(decode_add_order(m));
            break;
        case 'E':
            if constexpr (requires(Handler& hh, const OrderExecuted& e) {
                              hh.on_order_executed(e);
                          })
                h.on_order_executed(decode_order_executed(m));
            break;
        case 'C':
            if constexpr (requires(Handler& hh, const OrderExecutedWithPrice& e) {
                              hh.on_order_executed_with_price(e);
                          })
                h.on_order_executed_with_price(decode_order_executed_with_price(m));
            break;
        case 'X':
            if constexpr (requires(Handler& hh, const OrderCancel& e) { hh.on_order_cancel(e); })
                h.on_order_cancel(decode_order_cancel(m));
            break;
        case 'D':
            if constexpr (requires(Handler& hh, const OrderDelete& e) { hh.on_order_delete(e); })
                h.on_order_delete(decode_order_delete(m));
            break;
        case 'U':
            if constexpr (requires(Handler& hh, const OrderReplace& e) { hh.on_order_replace(e); })
                h.on_order_replace(decode_order_replace(m));
            break;
        case 'P':
            if constexpr (requires(Handler& hh, const Trade& e) { hh.on_trade(e); })
                h.on_trade(decode_trade(m));
            break;
        case 'Q':
            if constexpr (requires(Handler& hh, const CrossTrade& e) { hh.on_cross_trade(e); })
                h.on_cross_trade(decode_cross_trade(m));
            break;
        case 'B':
            if constexpr (requires(Handler& hh, const BrokenTrade& e) { hh.on_broken_trade(e); })
                h.on_broken_trade(decode_broken_trade(m));
            break;
        default: break;  // valid but not decoded (e.g. NOII); only on_message sees it
    }
    return true;
}

}  // namespace detail

// Parses every message in `buffer`, stopping at the first framing error.
template <class Handler>
ParseResult parse(std::span<const std::uint8_t> buffer, Handler& handler) {
    const std::uint8_t* const data = buffer.data();
    const std::size_t size = buffer.size();
    ParseResult result;
    std::size_t pos = 0;

    while (pos < size) {
        if (size - pos < 2) {
            result.error = ParseError::kTruncatedLength;
            break;
        }
        const std::size_t len = read_be16(data + pos);
        if (len == 0) {
            result.error = ParseError::kBadLength;
            break;
        }
        if (size - pos - 2 < len) {
            result.error = ParseError::kTruncatedMessage;
            break;
        }
        if (!detail::dispatch(data + pos + 2, len, handler)) {
            result.error = ParseError::kBadLength;
            break;
        }
        pos += 2 + len;
        ++result.messages;
    }

    result.bytes_consumed = pos;
    return result;
}

}  // namespace lob::itch
