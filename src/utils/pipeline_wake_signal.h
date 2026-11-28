#pragma once

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <mutex>
#include <memory>

namespace sr {

enum class PipelineWakeSource : std::size_t {
    CaptureFrame,
    MicrophoneAudio,
    LoopbackAudio,
    EncoderOutput,
    EncoderInputCapacity,
    EncoderFailure,
    Lifecycle,
    Count
};

// Shared wakeup for the recording consumer. Callers snapshot generation before
// inspecting queues, then wait for a change; this closes the check/sleep race.
class PipelineWakeSignal {
public:
    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    void notify(PipelineWakeSource source = PipelineWakeSource::Lifecycle) {
        const auto index = static_cast<std::size_t>(source);
        if (index < notification_counts_.size()) {
            notification_counts_[index].fetch_add(1, std::memory_order_relaxed);
        }
        bool should_wake = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            should_wake = waiter_active_;
        }
        if (should_wake) cv_.notify_one();
    }

    template <typename Rep, typename Period>
    bool wait_for_change(uint64_t observed,
                         std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (generation_ != observed) return true;
        waiter_active_ = true;
        const bool changed = cv_.wait_for(lock, timeout, [this, observed]() {
            return generation_ != observed;
        });
        waiter_active_ = false;
        return changed;
    }

    uint64_t notification_count(PipelineWakeSource source) const {
        const auto index = static_cast<std::size_t>(source);
        return index < notification_counts_.size()
            ? notification_counts_[index].load(std::memory_order_relaxed) : 0;
    }

    void reset_notification_counts() {
        for (auto& count : notification_counts_) count.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t generation_ = 0;
    bool waiter_active_ = false;
    std::array<std::atomic<uint64_t>, static_cast<std::size_t>(PipelineWakeSource::Count)>
        notification_counts_{};
};

} // namespace sr
