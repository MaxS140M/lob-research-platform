// Same-run comparison of the frozen v1 book (lob::reference::OrderBook) and the current
// lob::OrderBook, through identical workload and replay code.
//
//   LOB_ITCH_FILE=<file> taskset -c 2 ./lob_bench --benchmark_filter=Compare
//       --benchmark_enable_random_interleaving=true --benchmark_repetitions=5
//
// Random interleaving runs the repetitions of both versions in random order, so CPU clock
// drift during the run affects both equally.

#include <benchmark/benchmark.h>

#include <memory>

#include "lob/itch_parser.hpp"
#include "lob/order_book.hpp"
#include "reference_order_book.hpp"
#include "workload.hpp"

namespace {

using V1 = lob::reference::OrderBook;
using Current = lob::OrderBook;

template <class Book>
void BM_Compare_Add(benchmark::State& state) {
    lob::bench::run_book_add<Book>(state);
}
template <class Book>
void BM_Compare_Cancel(benchmark::State& state) {
    lob::bench::run_book_cancel<Book>(state);
}

template <class Book>
void BM_Compare_ReplayThroughput(benchmark::State& state) {
    const lob::MappedFile* file = lob::bench::itch_file();
    if (file == nullptr) {
        state.SkipWithError("set LOB_ITCH_FILE to an ITCH 5.0 file");
        return;
    }
    std::uint64_t messages = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto replayer = std::make_unique<lob::bench::Replayer<Book, false>>();
        state.ResumeTiming();

        messages += lob::itch::parse(file->bytes(), *replayer).messages;
        benchmark::DoNotOptimize(replayer->total_orders());

        state.PauseTiming();
        replayer.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(messages));
}

template <class Book>
void BM_Compare_ReplayLatency(benchmark::State& state) {
    const lob::MappedFile* file = lob::bench::itch_file();
    if (file == nullptr) {
        state.SkipWithError("set LOB_ITCH_FILE to an ITCH 5.0 file");
        return;
    }
    auto replayer = std::make_unique<lob::bench::Replayer<Book, true>>();
    for (auto _ : state) lob::itch::parse(file->bytes(), *replayer);
    lob::bench::report(state, replayer->add, "add_");
    lob::bench::report(state, replayer->execute, "exec_");
    lob::bench::report(state, replayer->cancel, "cancel_");
    lob::bench::report(state, replayer->remove, "delete_");
    lob::bench::report(state, replayer->replace, "replace_");
}

BENCHMARK_TEMPLATE(BM_Compare_Add, V1)->MinWarmUpTime(0.5)->MinTime(2.0);
BENCHMARK_TEMPLATE(BM_Compare_Add, Current)->MinWarmUpTime(0.5)->MinTime(2.0);
BENCHMARK_TEMPLATE(BM_Compare_Cancel, V1)->MinWarmUpTime(0.5)->MinTime(2.0);
BENCHMARK_TEMPLATE(BM_Compare_Cancel, Current)->MinWarmUpTime(0.5)->MinTime(2.0);
BENCHMARK_TEMPLATE(BM_Compare_ReplayThroughput, V1)
    ->Unit(benchmark::kMillisecond)
    ->MinWarmUpTime(2.0)
    ->Iterations(3)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_Compare_ReplayThroughput, Current)
    ->Unit(benchmark::kMillisecond)
    ->MinWarmUpTime(2.0)
    ->Iterations(3)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_Compare_ReplayLatency, V1)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_Compare_ReplayLatency, Current)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

}  // namespace
