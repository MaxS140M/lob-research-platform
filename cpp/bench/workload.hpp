#pragma once

// Shared benchmark workloads, templated on the book type so the current lob::OrderBook and the
// frozen v1 reference run exactly the same code.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "latency.hpp"
#include "lob/itch.hpp"
#include "lob/mapped_file.hpp"
#include "lob/types.hpp"

namespace lob::bench {

inline void report(benchmark::State& state, const LatencyHistogram& h,
                   const std::string& prefix = "") {
    state.counters[prefix + "p50_ns"] = h.percentile_ns(0.50);
    state.counters[prefix + "p99_ns"] = h.percentile_ns(0.99);
    state.counters[prefix + "p99.9_ns"] = h.percentile_ns(0.999);
    state.counters[prefix + "mean_ns"] = h.mean_ns();
}

// The ITCH file named by LOB_ITCH_FILE, mapped once per process, or nullptr if unset.
inline const MappedFile* itch_file() {
    static const std::unique_ptr<MappedFile> file = []() -> std::unique_ptr<MappedFile> {
        const char* path = std::getenv("LOB_ITCH_FILE");
        if (path == nullptr) return nullptr;
        return std::make_unique<MappedFile>(path);
    }();
    return file.get();
}

// ---- Synthetic book workload ---------------------------------------------
// A book shaped like a liquid stock: 1,000 resting orders of 100 shares spread over 50 price
// levels per side, one tick apart, around a $100 mid.

inline constexpr Price kMid = 1'000'000;  // $100.0000
inline constexpr Price kTick = 100;       // $0.01
inline constexpr std::uint32_t kLevelsPerSide = 50;
inline constexpr std::size_t kRestingOrders = 1'000;
inline constexpr Quantity kLot = 100;

class SyntheticFlow {
public:
    explicit SyntheticFlow(std::uint64_t seed) : rng_(seed) {}

    Side side() { return (rng_() & 1) == 0 ? Side::kBuy : Side::kSell; }

    Price passive_price(Side s) {
        const auto offset = static_cast<Price>(1 + rng_() % kLevelsPerSide);
        return s == Side::kBuy ? kMid - offset * kTick : kMid + offset * kTick;
    }

    std::size_t index(std::size_t n) { return static_cast<std::size_t>(rng_() % n); }

    OrderId next_id() { return next_id_++; }

private:
    std::mt19937_64 rng_;
    OrderId next_id_ = 1;
};

// Book plus the list of live ids, so a random live order can be picked in O(1).
template <class Book>
struct LiveBook {
    Book book;
    std::vector<OrderId> live;
    SyntheticFlow flow{42};

    LiveBook() {
        live.reserve(kRestingOrders * 2);
        while (live.size() < kRestingOrders) add_random();
    }

    void add_random() {
        const Side s = flow.side();
        const OrderId id = flow.next_id();
        book.add(id, s, flow.passive_price(s), kLot);
        live.push_back(id);
    }

    OrderId take_random() {
        const std::size_t i = flow.index(live.size());
        const OrderId id = live[i];
        live[i] = live.back();
        live.pop_back();
        return id;
    }
};

// Passive add into the synthetic book; a random order is removed afterwards (untimed) so
// the book size stays fixed.
template <class Book>
void run_book_add(benchmark::State& state) {
    LiveBook<Book> b;
    LatencyHistogram h;
    for (auto _ : state) {
        const Side s = b.flow.side();
        const Price price = b.flow.passive_price(s);
        const OrderId id = b.flow.next_id();

        const std::uint64_t t0 = tsc_start();
        b.book.add(id, s, price, kLot);
        const std::uint64_t t1 = tsc_stop();
        h.record(t1 - t0);

        b.live.push_back(id);
        b.book.remove(b.take_random());
    }
    report(state, h);
}

// Cancel a random resting order; it is replaced afterwards (untimed).
template <class Book>
void run_book_cancel(benchmark::State& state) {
    LiveBook<Book> b;
    LatencyHistogram h;
    for (auto _ : state) {
        const OrderId id = b.take_random();

        const std::uint64_t t0 = tsc_start();
        b.book.remove(id);
        const std::uint64_t t1 = tsc_stop();
        h.record(t1 - t0);

        b.add_random();
    }
    report(state, h);
}

// ---- Minimal ITCH replayer, templated on the book ------------------------
// Routes order messages to one Book per stock locate. Unlike BookBuilder it keeps no anomaly
// counts, so it measures only the book. With kTimed, every book update is timed and recorded
// by message type.

template <class Book, bool kTimed>
class Replayer {
public:
    LatencyHistogram add, execute, cancel, remove, replace;

    void on_add_order(const itch::AddOrder& m) {
        timed(add, [&] {
            book_for(m.stock_locate)
                .add(m.order_ref, m.side == 'B' ? Side::kBuy : Side::kSell, m.price, m.shares);
        });
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        timed(execute, [&] { books_[m.stock_locate].reduce(m.order_ref, m.executed_shares); });
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        timed(execute, [&] { books_[m.stock_locate].reduce(m.order_ref, m.executed_shares); });
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        timed(cancel, [&] { books_[m.stock_locate].reduce(m.order_ref, m.cancelled_shares); });
    }
    void on_order_delete(const itch::OrderDelete& m) {
        timed(remove, [&] { books_[m.stock_locate].remove(m.order_ref); });
    }
    void on_order_replace(const itch::OrderReplace& m) {
        timed(replace, [&] {
            books_[m.stock_locate].replace(m.original_order_ref, m.new_order_ref, m.price,
                                           m.shares);
        });
    }

    [[nodiscard]] std::size_t total_orders() const {
        std::size_t n = 0;
        for (const auto& b : books_) n += b.order_count();
        return n;
    }

private:
    template <class F>
    static void timed(LatencyHistogram& h, F&& f) {
        if constexpr (kTimed) {
            const std::uint64_t t0 = tsc_start();
            f();
            h.record(tsc_stop() - t0);
        } else {
            f();
        }
    }

    Book& book_for(std::uint16_t locate) {
        if (locate >= books_.size()) books_.resize(std::size_t{locate} + 1);
        return books_[locate];
    }

    // Every order message after the add refers to a live order on a known locate in a
    // well-formed file (book_replay checks this), so non-add messages index directly.
    std::vector<Book> books_;
};

}  // namespace lob::bench
