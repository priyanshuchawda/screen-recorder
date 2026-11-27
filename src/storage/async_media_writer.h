#pragma once

#include <atomic>
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
        std::lock_guard lock(lifecycle_mutex_);
        if (started_.load(std::memory_order_acquire) || worker_.joinable() || !write) return false;
        queue_.reset();
        samples_accepted_.store(0, std::memory_order_relaxed);
        samples_written_.store(0, std::memory_order_relaxed);
        write_failures_.store(0, std::memory_order_relaxed);
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
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (!started_.load(std::memory_order_acquire) && !worker_.joinable()) {
                return !failed_.load(std::memory_order_acquire);
            }
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
        bool failed = false;
        bool started = false;
    };

    Snapshot snapshot() const {
        return {queue_.snapshot(), samples_accepted_.load(std::memory_order_relaxed),
                samples_written_.load(std::memory_order_relaxed),
                write_failures_.load(std::memory_order_relaxed),
                failed_.load(std::memory_order_acquire),
                started_.load(std::memory_order_acquire)};
    }

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
            if (!succeeded) {
                failed_.store(true, std::memory_order_release);
                write_failures_.fetch_add(1, std::memory_order_relaxed);
                queue_.close_and_discard();
                break;
            }
            samples_written_.fetch_add(1, std::memory_order_relaxed);
        }

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

    BoundedMediaQueue<Payload> queue_;
    mutable std::mutex lifecycle_mutex_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    std::thread worker_;
    std::atomic<uint64_t> samples_accepted_{0};
    std::atomic<uint64_t> samples_written_{0};
    std::atomic<uint64_t> write_failures_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> started_{false};
    bool worker_ready_ = false;
    bool worker_setup_succeeded_ = false;
};

} // namespace sr
