#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sr {

// Shared wakeup for the recording consumer. Callers snapshot generation before
// inspecting queues, then wait for a change; this closes the check/sleep race.
class PipelineWakeSignal {
public:
    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    void notify() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
        }
        cv_.notify_all();
    }

    template <typename Rep, typename Period>
    bool wait_for_change(uint64_t observed,
                         std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, observed]() {
            return generation_ != observed;
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t generation_ = 0;
};

} // namespace sr
