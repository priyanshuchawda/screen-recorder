#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "utils/latency_histogram.h"

namespace sr {

// Controller-thread-owned accounting for periods when the bounded encoder
// input queue is full. It deliberately contains no wait primitive: the
// controller continues servicing audio, output and lifecycle events.
class EncoderBackpressureMetrics {
public:
    using Clock = std::chrono::steady_clock;
    enum class Resolution { Recovered, Cancelled, TimedOut };

    struct Snapshot {
        uint64_t attempts = 0;
        uint64_t recoveries = 0;
        uint64_t cancellations = 0;
        uint64_t timeouts = 0;
        uint64_t max_wait_us = 0;
        LatencyHistogram::Snapshot wait_us{};
    };

    bool begin_wait(Clock::time_point now) noexcept {
        if (waiting_) return false;
        waiting_ = true;
        started_ = now;
        ++snapshot_.attempts;
        return true;
    }

    void reset() noexcept {
        waiting_ = false;
        started_ = {};
        snapshot_ = {};
        wait_histogram_.reset();
    }

    bool waiting() const noexcept { return waiting_; }

    uint64_t elapsed_us(Clock::time_point now) const noexcept {
        if (!waiting_) return 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - started_).count();
        return static_cast<uint64_t>((std::max)(int64_t{0}, elapsed));
    }

    void resolve(Resolution result, Clock::time_point now) noexcept {
        if (!waiting_) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - started_).count();
        const uint64_t duration = static_cast<uint64_t>((std::max)(int64_t{0}, elapsed));
        wait_histogram_.record(duration);
        snapshot_.max_wait_us = (std::max)(snapshot_.max_wait_us, duration);
        switch (result) {
            case Resolution::Recovered: ++snapshot_.recoveries; break;
            case Resolution::Cancelled: ++snapshot_.cancellations; break;
            case Resolution::TimedOut: ++snapshot_.timeouts; break;
        }
        waiting_ = false;
    }

    Snapshot snapshot() const noexcept {
        Snapshot result = snapshot_;
        result.wait_us = wait_histogram_.snapshot();
        return result;
    }

private:
    bool waiting_ = false;
    Clock::time_point started_{};
    Snapshot snapshot_{};
    LatencyHistogram wait_histogram_;
};

} // namespace sr
