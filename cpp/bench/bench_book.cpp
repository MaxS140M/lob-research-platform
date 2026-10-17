// Order book and matching engine benchmarks.
//
//   Latency (per operation, p50 / p99 / p99.9 from a TSC histogram):
//     BM_TimerOverhead      cost of the timing itself, for reference
//     BM_BookAdd            passive add into a book of ~1,000 resting orders
//     BM_BookCancel         cancel a random resting order
//     BM_EngineMatch        marketable IOC that fills 1-3 resting orders
//     BM_ReplayLatency      every book update in a full ITCH day, by message type
//
//   Throughput:
//     BM_ReplayThroughput   full ITCH day: parse + rebuild every book, messages per second
//
// The ITCH benchmarks need LOB_ITCH_FILE=<path to an uncompressed ITCH 5.0 file>.
// For stable numbers, run pinned to one core:  taskset -c 2 ./lob_bench

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "latency.hpp"
#include "lob/book_builder.hpp"
#include "lob/itch_parser.hpp"
#include "lob/mapped_file.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"
#include "workload.hpp"

namespace itch = lob::itch;
using lob::bench::LatencyHistogram;
using lob::bench::tsc_start;
using lob::bench::tsc_stop;

namespace {

using lob::bench::kLevelsPerSide;
using lob::bench::kLot;
using lob::bench::kMid;
using lob::bench::kRestingOrders;
using lob::bench::kTick;
using lob::bench::report;
using lob::bench::SyntheticFlow;

void BM_TimerOverhead(benchmark::State& state) {
    LatencyHistogram h;
    for (auto _ : state) {
        const std::uint64_t t0 = tsc_start();
        const std::uint64_t t1 = tsc_stop();
        h.record(t1 - t0);
    }
    report(state, h);
}
BENCHMARK(BM_TimerOverhead)->MinWarmUpTime(0.5);

void BM_BookAdd(benchmark::State& state) { lob::bench::run_book_add<lob::OrderBook>(state); }
BENCHMARK(BM_BookAdd)->MinWarmUpTime(0.5)->MinTime(2.0);

void BM_BookCancel(benchmark::State& state) { lob::bench::run_book_cancel<lob::OrderBook>(state); }
BENCHMARK(BM_BookCancel)->MinWarmUpTime(0.5)->MinTime(2.0);

void BM_EngineMatch(benchmark::State& state) {
    lob::MatchingEngine engine;
    SyntheticFlow flow(7);
    for (std::size_t i = 0; i < kRestingOrders; ++i) {
        const lob::Side s = flow.side();
        engine.submit({flow.next_id(), s, lob::OrderType::kLimit, flow.passive_price(s), kLot});
    }

    struct FillBuffer {
        std::array<lob::Fill, 16> fills;
        std::size_t n = 0;
    } buffer;
    LatencyHistogram h;

    for (auto _ : state) {
        const lob::Side s = flow.side();
        // Marketable IOC for 1-3 lots: always crosses, fills 1-3 orders at the touch.
        const lob::Price limit =
            s == lob::Side::kBuy ? kMid + kLevelsPerSide * kTick : kMid - kLevelsPerSide * kTick;
        const lob::Quantity qty = kLot * static_cast<lob::Quantity>(1 + flow.index(3));
        const lob::NewOrder order{flow.next_id(), s, lob::OrderType::kImmediateOrCancel, limit,
                                  qty};
        buffer.n = 0;

        const std::uint64_t t0 = tsc_start();
        auto result =
            engine.submit(order, [&](const lob::Fill& f) { buffer.fills[buffer.n++] = f; });
        const std::uint64_t t1 = tsc_stop();
        h.record(t1 - t0);
        benchmark::DoNotOptimize(result);

        // Put back what was taken so the book keeps its shape (untimed).
        for (std::size_t i = 0; i < buffer.n; ++i) {
            const lob::Fill& f = buffer.fills[i];
            engine.submit(
                {flow.next_id(), lob::opposite(s), lob::OrderType::kLimit, f.price, f.quantity});
        }
    }
    report(state, h);
}
BENCHMARK(BM_EngineMatch)->MinWarmUpTime(0.5)->MinTime(2.0);

// ---- Real ITCH data ------------------------------------------------------

using lob::bench::itch_file;

void BM_ReplayThroughput(benchmark::State& state) {
    const lob::MappedFile* file = itch_file();
    if (file == nullptr) {
        state.SkipWithError("set LOB_ITCH_FILE to an ITCH 5.0 file");
        return;
    }
    std::uint64_t messages = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto builder = std::make_unique<lob::BookBuilder>();
        state.ResumeTiming();

        const auto result = itch::parse(file->bytes(), *builder);
        benchmark::DoNotOptimize(builder->anomalies().total());
        messages += result.messages;

        state.PauseTiming();
        builder.reset();  // tearing down 8,000+ books is not part of the replay
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(messages));
    state.counters["ns_per_msg"] = benchmark::Counter(
        static_cast<double>(messages), benchmark::Counter::kIsRate | benchmark::Counter::kInvert,
        benchmark::Counter::OneK::kIs1000);
}
BENCHMARK(BM_ReplayThroughput)
    ->Unit(benchmark::kMillisecond)
    ->MinWarmUpTime(2.0)
    ->Iterations(5)
    ->UseRealTime();

// Times the book update for every order message in a full day, per message type.
class TimedReplay : public lob::BookBuilder {
public:
    LatencyHistogram add, execute, cancel, remove, replace;

    void on_add_order(const itch::AddOrder& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_add_order(m);
        add.record(tsc_stop() - t0);
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_order_executed(m);
        execute.record(tsc_stop() - t0);
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_order_executed_with_price(m);
        execute.record(tsc_stop() - t0);
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_order_cancel(m);
        cancel.record(tsc_stop() - t0);
    }
    void on_order_delete(const itch::OrderDelete& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_order_delete(m);
        remove.record(tsc_stop() - t0);
    }
    void on_order_replace(const itch::OrderReplace& m) {
        const auto t0 = tsc_start();
        BookBuilder::on_order_replace(m);
        replace.record(tsc_stop() - t0);
    }
};

void BM_ReplayLatency(benchmark::State& state) {
    const lob::MappedFile* file = itch_file();
    if (file == nullptr) {
        state.SkipWithError("set LOB_ITCH_FILE to an ITCH 5.0 file");
        return;
    }
    {
        lob::BookBuilder warm;  // fault the file into the page cache first
        itch::parse(file->bytes(), warm);
    }
    auto replay = std::make_unique<TimedReplay>();
    for (auto _ : state) {
        itch::parse(file->bytes(), *replay);
    }
    report(state, replay->add, "add_");
    report(state, replay->execute, "exec_");
    report(state, replay->cancel, "cancel_");
    report(state, replay->remove, "delete_");
    report(state, replay->replace, "replace_");
}
BENCHMARK(BM_ReplayLatency)->Unit(benchmark::kMillisecond)->Iterations(1)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
