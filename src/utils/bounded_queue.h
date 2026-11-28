#pragma once
// bounded_queue.h — Bounded MPSC queue (multi-producer, single-consumer)
// Push and pop operations share a mutex so the real-time video producer can
// safely discard the oldest queued frame when it needs to keep video current.
// Max depth is statically bounded by Capacity — queues NEVER grow unboundedly.
//
// T036 Memory Stability Review:
//   - Video queue:  Capacity=8  → max 8 queued D3D11 texture refs = bounded
//   - Audio queues retain a bounded reserve for temporary muxer stalls.
//   - All queues use std::array<T, Capacity+1> — FIXED stack/BSS allocation at startup
//   - try_push returns false (caller drops) when full — NO dynamic growth
//   - COM texture pointers in RenderFrame use WRL ComPtr — Released in destructor
//     when popped frame goes out of scope in encode_loop
//   - AudioPacket.buffer is std::vector<uint8_t> — bounded by PCM packet size (~1KB)
//   - No global statics with COM objects (COM uninit-safe)
//
// Architectural constraint check:
//   BoundedQueue<RenderFrame, 8>  ← short fixed backlog for transient encoder stalls

#include <atomic>
#include <optional>
#include <array>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "utils/pipeline_wake_signal.h"

namespace sr {

template <typename T, size_t Capacity = 5>
class BoundedQueue {
    // T036: Compile-time guard — prevent accidental unbounded config
    static_assert(Capacity >= 1  && Capacity <= 2048,
                  "BoundedQueue Capacity must be in [1, 2048]");

public:
    BoundedQueue() : head_(0), tail_(0) {}

    // Install before starting queue producers. The signal must outlive them.
    void set_wake_signal(PipelineWakeSignal* signal) {
        wake_signal_.store(signal, std::memory_order_release);
    }

    // Non-blocking push. Returns false if queue is full (caller applies drop policy).
    // Thread-safe for multiple concurrent producers (mutex-protected).
    bool try_push(T&& item) {
        std::unique_lock<std::mutex> lock(push_mutex_);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % (Capacity + 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // Full — caller must apply drop policy
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        update_high_watermark(size());
        lock.unlock();
        cv_.notify_one();
        notify_pipeline();
        return true;
    }

    // Non-blocking pop. Returns nullopt if queue is empty.
    // Safe against producers that use either push policy below.
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(push_mutex_);
        return pop_locked();
    }

    // Blocking pop with timeout. The queue mutex also protects the condition
    // predicate, so a push cannot slip between the empty check and sleeping.
    std::optional<T> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(push_mutex_);
        if (!cv_.wait_for(lock, timeout, [this]() {
                return head_.load(std::memory_order_acquire) !=
                       tail_.load(std::memory_order_acquire);
            })) {
            return std::nullopt;
        }
        return pop_locked();
    }

private:
    std::optional<T> pop_locked() {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Empty
        }
        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
        return item;
    }

public:

    // Keep the newest item when full by dropping the oldest queued item.
    // Intended for real-time video, where a current frame is more useful than
    // encoding a stale backlog. Reports whether an old item was discarded.
    bool try_push_latest(T&& item, bool* dropped_oldest = nullptr) {
        std::unique_lock<std::mutex> lock(push_mutex_);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % (Capacity + 1);
        bool dropped = false;
        if (next == tail_.load(std::memory_order_acquire)) {
            size_t tail = tail_.load(std::memory_order_relaxed);
            buffer_[tail] = T{};
            tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
            dropped = true;
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        update_high_watermark(size());
        if (dropped_oldest) *dropped_oldest = dropped;
        lock.unlock();
        cv_.notify_one();
        notify_pipeline();
        return true;
    }

    // Current occupancy (approximate — head/tail may race)
    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h >= t) ? (h - t) : (Capacity + 1 - t + h);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    bool full() const {
        size_t next = (head_.load(std::memory_order_acquire) + 1) % (Capacity + 1);
        return next == tail_.load(std::memory_order_acquire);
    }

    // T036: expose capacity for runtime assertions and telemetry
    static constexpr size_t capacity() { return Capacity; }

    // Highest observed occupancy since construction or explicit reset.
    size_t high_watermark() const {
        return high_watermark_.load(std::memory_order_relaxed);
    }

    void reset_high_watermark() {
        high_watermark_.store(size(), std::memory_order_relaxed);
    }

private:
    void notify_pipeline() {
        if (auto* signal = wake_signal_.load(std::memory_order_acquire)) {
            signal->notify();
        }
    }

    void update_high_watermark(size_t occupancy) {
        size_t observed = high_watermark_.load(std::memory_order_relaxed);
        while (occupancy > observed &&
               !high_watermark_.compare_exchange_weak(observed, occupancy,
                                                      std::memory_order_relaxed)) {}
    }

    // T036: Fixed-size ring buffer — NEVER allocates after construction
    std::array<T, Capacity + 1> buffer_; // One extra slot for ring buffer sentinel
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::atomic<size_t> high_watermark_{ 0 };
    std::mutex push_mutex_; // Serializes concurrent producers
    std::condition_variable cv_;
    std::atomic<PipelineWakeSignal*> wake_signal_{ nullptr };
};

} // namespace sr
