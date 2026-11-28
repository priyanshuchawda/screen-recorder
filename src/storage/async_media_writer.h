#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "storage/bounded_media_queue.h"

namespace sr {

// Single-consumer writer that keeps sink operations off capture/audio
// coordination threads. Queue saturation is returned to the producer; accepted
// media is drained on close and never evicted silently.
template <typename Payload>
class AsyncMediaWriter {
public:
    using Packet = MediaPacket<Payload>;
    using WriteCallback = std::function<bool(Packet&)>;
    using WorkerSetup = std::function<bool()>;
    using WorkerTeardown = std::function<void()>;

    explicit AsyncMediaWriter(const MediaQueueLimits& limits = MediaQueueLimits{})
        : queue_(limits) {}
    ~AsyncMediaWriter() { abort(); }

    AsyncMediaWriter(const AsyncMediaWriter&) = delete;
    AsyncMediaWriter& operator=(const AsyncMediaWriter&) = delete;

    bool start(WriteCallback write, WorkerSetup setup = {}, WorkerTeardown teardown = {}) {
        std::lock_guard operation_lock(operation_mutex_);
        std::lock_guard lock(lifecycle_mutex_);
        if (started_.load(std::memory_order_acquire) || worker_.joinable() || !write) return false;
        queue_.reset();
        samples_accepted_.store(0, std::memory_order_relaxed);
        samples_written_.store(0, std::memory_order_relaxed);
        write_failures_.store(0, std::memory_order_relaxed);
        drain_ms_.store(0, std::memory_order_relaxed);
        close_requested_ns_.store(0, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_release);
        {
            std::lock_guard ready_lock(ready_mutex_);
            worker_ready_ = false;
            worker_setup_succeeded_ = false;
        }
        started_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread([this, write = std::move(write),
                                   setup = std::move(setup), teardown = std::move(teardown)]() mutable {
                worker_loop(std::move(write), std::move(setup), std::move(teardown));
            });
        } catch (...) {
            started_.store(false, std::memory_order_release);
            queue_.close_and_discard();
            return false;
        }
        std::unique_lock ready_lock(ready_mutex_);
        ready_cv_.wait(ready_lock, [this] { return worker_ready_; });
        const bool setup_succeeded = worker_setup_succeeded_;
        ready_lock.unlock();
        if (!setup_succeeded) {
            started_.store(false, std::memory_order_release);
            if (worker_.joinable()) worker_.join();
        }
        return setup_succeeded;
    }

    MediaQueueResult enqueue(Packet&& packet) {
        if (!started_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire)) {
            return MediaQueueResult::Closed;
        }
        const auto result = queue_.try_push(std::move(packet));
        if (result == MediaQueueResult::Accepted) {
            samples_accepted_.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    bool finish() {
        std::lock_guard operation_lock(operation_mutex_);
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (!started_.load(std::memory_order_acquire) && !worker_.joinable()) {
                return !failed_.load(std::memory_order_acquire);
            }
            close_requested_ns_.store(now_ns(), std::memory_order_release);
            queue_.close();
            started_.store(false, std::memory_order_release);
        }
        if (worker_.joinable()) worker_.join();
        const auto state = snapshot();
        return !state.failed && state.queue.packet_count == 0 &&
               state.samples_accepted == state.samples_written &&
               state.queue.discarded_packets == 0;
    }

    void abort() {
        std::lock_guard operation_lock(operation_mutex_);
        {
            std::lock_guard lock(lifecycle_mutex_);
            queue_.close_and_discard();
            started_.store(false, std::memory_order_release);
        }
        if (worker_.joinable()) worker_.join();
    }

    struct Snapshot {
        MediaQueueSnapshot queue;
        uint64_t samples_accepted = 0;
        uint64_t samples_written = 0;
        uint64_t write_failures = 0;
        uint64_t drain_ms = 0;
        bool failed = false;
        bool started = false;
    };

    Snapshot snapshot() const {
        return {queue_.snapshot(), samples_accepted_.load(std::memory_order_relaxed),
                samples_written_.load(std::memory_order_relaxed),
                write_failures_.load(std::memory_order_relaxed),
                drain_ms_.load(std::memory_order_relaxed),
                failed_.load(std::memory_order_acquire),
                started_.load(std::memory_order_acquire)};
    }

    bool failed() const { return failed_.load(std::memory_order_acquire); }

private:
    void worker_loop(WriteCallback write, WorkerSetup setup, WorkerTeardown teardown) {
        bool setup_ok = true;
        try {
            if (setup) setup_ok = setup();
        } catch (...) {
            setup_ok = false;
        }
        if (!setup_ok) {
            failed_.store(true, std::memory_order_release);
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            queue_.close_and_discard();
            try {
                if (teardown) teardown();
            } catch (...) {
                write_failures_.fetch_add(1, std::memory_order_relaxed);
            }
            signal_worker_ready(false);
            return;
        }
        signal_worker_ready(true);

        while (auto packet = queue_.wait_pop()) {
            bool succeeded = false;
            try {
                succeeded = write(*packet);
            } catch (...) {
                succeeded = false;
            }
            // Drop the last sample reference before releasing its active byte
            // reservation so producers cannot temporarily exceed the budget.
            packet->payload = Payload{};
            queue_.release_active(*packet);
            if (!succeeded) {
                failed_.store(true, std::memory_order_release);
                write_failures_.fetch_add(1, std::memory_order_relaxed);
                queue_.close_and_discard();
                break;
            }
            samples_written_.fetch_add(1, std::memory_order_relaxed);
        }

        int64_t drain_started = close_requested_ns_.load(std::memory_order_acquire);
        if (drain_started == 0) drain_started = now_ns();
        const uint64_t elapsed_ns = static_cast<uint64_t>((std::max)(int64_t{0}, now_ns() - drain_started));
        drain_ms_.store(elapsed_ns / 1'000'000, std::memory_order_release);

        try {
            if (teardown) teardown();
        } catch (...) {
            failed_.store(true, std::memory_order_release);
            write_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void signal_worker_ready(bool succeeded) {
        {
            std::lock_guard lock(ready_mutex_);
            worker_setup_succeeded_ = succeeded;
            worker_ready_ = true;
        }
        ready_cv_.notify_all();
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    BoundedMediaQueue<Payload> queue_;
    std::mutex operation_mutex_;
    mutable std::mutex lifecycle_mutex_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    std::thread worker_;
    std::atomic<uint64_t> samples_accepted_{0};
    std::atomic<uint64_t> samples_written_{0};
    std::atomic<uint64_t> write_failures_{0};
    std::atomic<uint64_t> drain_ms_{0};
    std::atomic<int64_t> close_requested_ns_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> started_{false};
    bool worker_ready_ = false;
    bool worker_setup_succeeded_ = false;
};

} // namespace sr
