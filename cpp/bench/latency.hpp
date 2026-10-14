#pragma once

// Per-operation latency measurement with the CPU timestamp counter.
//
// steady_clock costs ~20 ns per read, too coarse for operations that take tens of ns, so single
// operations are timed with rdtsc. Fences keep the timed code from being reordered across the
// reads (Intel's recommended lfence/rdtsc ... rdtscp/lfence pattern). Samples go into a
// histogram with one-cycle resolution, so percentiles are exact and recording costs one
// increment, not a vector push.

#include <x86intrin.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace lob::bench {

inline std::uint64_t tsc_start() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline std::uint64_t tsc_stop() noexcept {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// TSC ticks per nanosecond, i.e. the TSC frequency in GHz.
//
// The TSC runs at a fixed hardware rate, so the best source is that rate itself: set
// LOB_TSC_GHZ to it (Linux prints it at boot: `dmesg | grep "tsc: Detected"`). Otherwise it is
// calibrated against steady_clock. That is accurate on bare-metal Linux, but under WSL2 the
// guest clock is steered by the host's time sync and the calibration drifts by up to ~9%
// between runs, so the benchmarks in this repo set LOB_TSC_GHZ.
inline double tsc_ticks_per_ns() {
    static const double ticks = [] {
        if (const char* ghz = std::getenv("LOB_TSC_GHZ")) return std::strtod(ghz, nullptr);
        const auto t0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = tsc_start();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
        }
        const std::uint64_t c1 = tsc_stop();
        const auto ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0);
        return static_cast<double>(c1 - c0) / ns.count();
    }();
    return ticks;
}

class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 1 << 16;  // cycles; larger samples clamp to the top

    void record(std::uint64_t cycles) noexcept {
        ++counts_[cycles < kBuckets ? cycles : kBuckets - 1];
        ++total_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }

    // Smallest cycle count c such that at least fraction q of samples are <= c.
    [[nodiscard]] std::uint64_t percentile_cycles(double q) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(q * static_cast<double>(total_));
        std::uint64_t seen = 0;
        for (std::size_t c = 0; c < kBuckets; ++c) {
            seen += counts_[c];
            if (seen > target || (seen == total_)) return c;
        }
        return kBuckets - 1;
    }

    [[nodiscard]] double percentile_ns(double q) const {
        return static_cast<double>(percentile_cycles(q)) / tsc_ticks_per_ns();
    }

    [[nodiscard]] double mean_ns() const {
        if (total_ == 0) return 0;
        long double sum = 0;
        for (std::size_t c = 0; c < kBuckets; ++c) sum += static_cast<long double>(c) * counts_[c];
        return static_cast<double>(sum / total_) / tsc_ticks_per_ns();
    }

    // Number of samples that took exactly `cycles` cycles (the last bucket also holds slower ones).
    [[nodiscard]] std::uint64_t count_at(std::size_t cycles) const noexcept {
        return counts_[cycles];
    }

    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t c = 0; c < kBuckets; ++c) counts_[c] += other.counts_[c];
        total_ += other.total_;
    }

private:
    std::vector<std::uint64_t> counts_ = std::vector<std::uint64_t>(kBuckets, 0);
    std::uint64_t total_ = 0;
};

}  // namespace lob::bench
