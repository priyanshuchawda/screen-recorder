#pragma once
// qpc_clock.h — High-resolution QPC timing wrappers
// Provides nanosecond-resolution monotonic timestamps for A/V sync

#include <windows.h>
#include <cstdint>

namespace sr {

class QPCClock {
public:
    QPCClock() {
        QueryPerformanceFrequency(&freq_);
    }

    // Current time in 100-nanosecond units (matches Media Foundation)
    int64_t now_hns() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        // Convert to 100ns units: counter * 10,000,000 / freq
        return static_cast<int64_t>(
            (static_cast<double>(counter.QuadPart) / freq_.QuadPart) * 10'000'000.0
        );
    }

    // Current time in nanoseconds
    int64_t now_ns() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<int64_t>(
            (static_cast<double>(counter.QuadPart) / freq_.QuadPart) * 1'000'000'000.0
        );
    }

    // Current time in microseconds
    int64_t now_us() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<int64_t>(
            (static_cast<double>(counter.QuadPart) / freq_.QuadPart) * 1'000'000.0
        );
    }

    // Current time in milliseconds
    double now_ms() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return (static_cast<double>(counter.QuadPart) / freq_.QuadPart) * 1000.0;
    }

    // QPC frequency (ticks per second)
    int64_t frequency() const { return freq_.QuadPart; }

    // Convert QPC ticks to 100ns units
    int64_t ticks_to_hns(int64_t ticks) const {
        return static_cast<int64_t>(
            (static_cast<double>(ticks) / freq_.QuadPart) * 10'000'000.0
        );
    }

    // Singleton access for shared clock
    static const QPCClock& instance() {
        static QPCClock clock;
        return clock;
    }

private:
    LARGE_INTEGER freq_{};
};

} // namespace sr
