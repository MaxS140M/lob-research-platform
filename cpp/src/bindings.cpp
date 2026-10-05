// pybind11 bindings: the _lob extension module behind the lobpy package.
//
// Results come back as dicts of NumPy arrays. Each array is a view over a std::vector that C++
// filled during the replay: the vector is moved to the heap and a capsule owns it, so NumPy
// frees it when the array is garbage-collected and the data is never copied. The GIL is
// released for the replay itself, so other Python threads keep running.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lob/itch.hpp"
#include "lob/replayer.hpp"

namespace py = pybind11;

namespace {

// Hands ownership of `data` to a NumPy array of the given shape, without copying.
template <class T>
py::array_t<T> to_numpy(std::vector<T>&& data, std::vector<py::ssize_t> shape) {
    auto* owned = new std::vector<T>(std::move(data));
    py::capsule owner(owned, [](void* p) { delete static_cast<std::vector<T>*>(p); });
    return py::array_t<T>(std::move(shape), owned->data(), owner);
}

py::dict snapshots(const lob::Replayer& replayer, std::uint64_t interval_ns, std::size_t depth,
                   std::optional<std::uint64_t> start_ns, std::optional<std::uint64_t> end_ns) {
    lob::SnapshotData d;
    {
        py::gil_scoped_release release;
        d = replayer.snapshots(interval_ns, depth, start_ns, end_ns);
    }
    const auto rows = static_cast<py::ssize_t>(d.rows);
    const auto levels = static_cast<py::ssize_t>(d.depth);
    py::dict out;
    out["timestamp"] = to_numpy(std::move(d.timestamp), {rows});
    out["symbol"] = to_numpy(std::move(d.symbol), {rows});
    out["bid_price"] = to_numpy(std::move(d.bid_price), {rows, levels});
    out["bid_size"] = to_numpy(std::move(d.bid_size), {rows, levels});
    out["ask_price"] = to_numpy(std::move(d.ask_price), {rows, levels});
    out["ask_size"] = to_numpy(std::move(d.ask_size), {rows, levels});
    return out;
}

py::dict trades(const lob::Replayer& replayer) {
    lob::TradeData d;
    {
        py::gil_scoped_release release;
        d = replayer.trades();
    }
    const auto rows = static_cast<py::ssize_t>(d.timestamp.size());
    py::dict out;
    out["timestamp"] = to_numpy(std::move(d.timestamp), {rows});
    out["symbol"] = to_numpy(std::move(d.symbol), {rows});
    out["price"] = to_numpy(std::move(d.price), {rows});
    out["shares"] = to_numpy(std::move(d.shares), {rows});
    out["side"] = to_numpy(std::move(d.side), {rows});
    out["match_number"] = to_numpy(std::move(d.match_number), {rows});
    out["kind"] = to_numpy(std::move(d.kind), {rows});
    out["printable"] = to_numpy(std::move(d.printable), {rows});
    return out;
}

py::dict events(const lob::Replayer& replayer, const std::string& symbol, std::size_t depth,
                std::optional<std::uint64_t> start_ns, std::optional<std::uint64_t> end_ns) {
    lob::EventData d;
    {
        py::gil_scoped_release release;
        d = replayer.events(symbol, depth, start_ns, end_ns);
    }
    const auto rows = static_cast<py::ssize_t>(d.rows());
    const auto levels = static_cast<py::ssize_t>(d.depth);
    py::dict out;
    out["timestamp"] = to_numpy(std::move(d.timestamp), {rows});
    out["seq"] = to_numpy(std::move(d.seq), {rows});
    out["kind"] = to_numpy(std::move(d.kind), {rows});
    out["side"] = to_numpy(std::move(d.side), {rows});
    out["price"] = to_numpy(std::move(d.price), {rows});
    out["shares"] = to_numpy(std::move(d.shares), {rows});
    out["trade_price"] = to_numpy(std::move(d.trade_price), {rows});
    out["priority"] = to_numpy(std::move(d.priority), {rows});
    out["old_price"] = to_numpy(std::move(d.old_price), {rows});
    out["old_shares"] = to_numpy(std::move(d.old_shares), {rows});
    out["old_priority"] = to_numpy(std::move(d.old_priority), {rows});
    out["bid_price"] = to_numpy(std::move(d.bid_price), {rows, levels});
    out["bid_size"] = to_numpy(std::move(d.bid_size), {rows, levels});
    out["ask_price"] = to_numpy(std::move(d.ask_price), {rows, levels});
    out["ask_size"] = to_numpy(std::move(d.ask_size), {rows, levels});
    return out;
}

}  // namespace

PYBIND11_MODULE(_lob, m) {
    m.doc() = "C++ limit order book engine: ITCH 5.0 replay into NumPy arrays";
    m.attr("PRICE_SCALE") = lob::itch::kPriceScale;

    py::class_<lob::Replayer>(m, "Replayer", R"doc(
Replays one ITCH 5.0 file through C++ order books for a set of symbols.

    Replayer(path, symbols=[])

An empty symbol list means every symbol in the file. Raises ValueError for a symbol that is
not in the file.
)doc")
        .def(py::init<const std::filesystem::path&, std::vector<std::string>>(), py::arg("path"),
             py::arg("symbols") = std::vector<std::string>{})
        .def_property_readonly("symbols", &lob::Replayer::symbols)
        .def_property_readonly("first_timestamp", &lob::Replayer::first_timestamp,
                               "ns since midnight of the first message")
        .def_property_readonly("last_timestamp", &lob::Replayer::last_timestamp,
                               "ns since midnight of the last message")
        .def("snapshots", &snapshots, py::arg("interval_ns"), py::arg("depth") = 5,
             py::arg("start_ns") = py::none(), py::arg("end_ns") = py::none(), R"doc(
Top-of-book snapshots every interval_ns, for the top `depth` levels.

Each snapshot reflects every message with timestamp <= its sample time. Returns a dict of
NumPy arrays: timestamp [n] (ns since midnight), symbol [n] (index into .symbols), and
bid_price, bid_size, ask_price, ask_size [n, depth], best level first. Missing levels are 0.
Prices are int64 with 4 implied decimals (divide by PRICE_SCALE for dollars).
)doc")
        .def("events", &events, py::arg("symbol"), py::arg("depth") = 5,
             py::arg("start_ns") = py::none(), py::arg("end_ns") = py::none(), R"doc(
Every book event for one symbol, in file order: the feed for an event-driven backtest.

One row per message touching the symbol: timestamp, seq (per-symbol message counter), kind
(ITCH type byte: A, E, C, X, D, U, P, Q), side (+1 bid / -1 ask of the resting order; 0 for
crosses), price (its level), shares, trade_price (E/C/P/Q), priority (seq at which the order
took its queue position; a replace gets a new one), old_price / old_shares / old_priority
(replaces only), and bid/ask price and size [n, depth] after the event.
)doc")
        .def("trades", &trades, R"doc(
Every execution for the chosen symbols, in file order.

Returns a dict of NumPy arrays: timestamp, symbol, price (4 implied decimals), shares,
side (aggressor: +1 buyer-initiated, -1 seller-initiated, 0 for crosses), match_number,
kind (ITCH type: E, C, P or Q as a byte), printable (0 for non-printable executions).
)doc");
}
