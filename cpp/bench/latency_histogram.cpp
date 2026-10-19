// latency_histogram: the full distribution of book-update latency on a real ITCH day, for the
// original (v1) book and the current one. Writes CSV for scripts/make_charts.py.
//
//   usage: taskset -c 2 latency_histogram <itch-file> > latency.csv
//
// Every add, execute, cancel, delete and replace is timed individually with rdtsc (see
// latency.hpp), exactly as in BM_Compare_ReplayLatency, and all of them go in one histogram
// per book version. The two versions are replayed alternately, several times each, and the
// runs are merged: a burst of background activity on the machine then affects both versions
// equally instead of distorting whichever happened to be running.

#include <cstdio>
#include <exception>

#include "latency.hpp"
#include "lob/itch_parser.hpp"
#include "lob/mapped_file.hpp"
#include "lob/order_book.hpp"
#include "reference_order_book.hpp"
#include "workload.hpp"

namespace {

template <class Book>
lob::bench::LatencyHistogram replay(const lob::MappedFile& file) {
    lob::bench::Replayer<Book, true> r;
    lob::itch::parse(file.bytes(), r);
    lob::bench::LatencyHistogram all;
    for (const auto* h : {&r.add, &r.execute, &r.cancel, &r.remove, &r.replace}) all.merge(*h);
    return all;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-5.0-file>\n", argv[0]);
        return 2;
    }
    try {
        const lob::MappedFile file(argv[1]);
        {
            lob::bench::Replayer<lob::OrderBook, false> warm;  // page the file into memory
            lob::itch::parse(file.bytes(), warm);
        }
        constexpr int kRounds = 5;
        lob::bench::LatencyHistogram v1;
        lob::bench::LatencyHistogram current;
        for (int round = 0; round < kRounds; ++round) {
            v1.merge(replay<lob::reference::OrderBook>(file));
            current.merge(replay<lob::OrderBook>(file));
        }

        std::printf("# ticks_per_ns=%.6f\n", lob::bench::tsc_ticks_per_ns());
        std::printf("cycles,v1,current\n");
        for (std::size_t c = 0; c < lob::bench::LatencyHistogram::kBuckets; ++c) {
            const auto a = v1.count_at(c);
            const auto b = current.count_at(c);
            if (a || b) {
                std::printf("%zu,%llu,%llu\n", c, static_cast<unsigned long long>(a),
                            static_cast<unsigned long long>(b));
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
