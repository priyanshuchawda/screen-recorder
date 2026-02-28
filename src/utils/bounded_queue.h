#pragma once
// bounded_queue.h — Bounded MPSC queue (multi-producer, single-consumer)
// Push side is protected by a spinlock to allow concurrent producers.
// Pop side is lock-free (single consumer assumed).
// Max depth configurable (default 5 for video, 10 for audio)

#include <atomic>
#include <optional>
#include <array>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>

namespace sr {

template <typename T, size_t Capacity = 5>
class BoundedQueue {
public:
    BoundedQueue() : head_(0), tail_(0) {}

    // Non-blocking push. Returns false if queue is full (caller applies drop policy).
    // Thread-safe for multiple concurrent producers (mutex-protected).
    bool try_push(T&& item) {
        std::lock_guard<std::mutex> lock(push_mutex_);
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % (Capacity + 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // Full
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Non-blocking pop. Returns nullopt if queue is empty
    std::optional<T> try_pop() {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Empty
        }
        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
        return item;
    }

    // Blocking pop with timeout. Returns nullopt on timeout
    std::optional<T> wait_pop(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto item = try_pop();
            if (item.has_value()) return item;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return std::nullopt;
    }

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

    static constexpr size_t capacity() { return Capacity; }

private:
    std::array<T, Capacity + 1> buffer_; // One extra slot for ring buffer
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::mutex push_mutex_; // Serializes concurrent producers
};

} // namespace sr
