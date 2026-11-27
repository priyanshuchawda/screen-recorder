#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace sr {

// Fixed-memory, logarithmic microsecond histogram. Bucket i covers (2^(i-1), 2^i]
// (bucket zero covers 0..1), so recording duration cannot grow its memory use.
class LatencyHistogram {
public:
    static constexpr size_t kBucketCount = 64;
    struct Snapshot {
        uint64_t samples = 0;
        uint64_t p50_upper_us = 0;
        uint64_t p95_upper_us = 0;
        uint64_t p99_upper_us = 0;
    };

    void record(uint64_t duration_us) noexcept {
        const size_t bucket = duration_us == 0 ? 0
            : (std::min)(kBucketCount - 1,
                static_cast<size_t>(std::bit_width(duration_us - 1)));
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        samples_.fetch_add(1, std::memory_order_relaxed);
    }

    Snapshot snapshot() const noexcept {
        Snapshot result{};
        result.samples = samples_.load(std::memory_order_relaxed);
        if (result.samples == 0) return result;
        result.p50_upper_us = percentile_upper(50, result.samples);
        result.p95_upper_us = percentile_upper(95, result.samples);
        result.p99_upper_us = percentile_upper(99, result.samples);
        return result;
    }

    void reset() noexcept {
        for (auto& bucket : buckets_) bucket.store(0, std::memory_order_relaxed);
        samples_.store(0, std::memory_order_relaxed);
    }

private:
    uint64_t percentile_upper(uint32_t percentile, uint64_t total) const noexcept {
        const uint64_t rank = (total * percentile + 99) / 100;
        uint64_t seen = 0;
        for (size_t i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i].load(std::memory_order_relaxed);
            if (seen >= rank) {
                if (i == 0) return 1;
                if (i >= 63) return UINT64_MAX;
                return uint64_t{1} << i;
            }
        }
        return UINT64_MAX;
    }

    std::array<std::atomic<uint64_t>, kBucketCount> buckets_{};
    std::atomic<uint64_t> samples_{ 0 };
};

} // namespace sr
