#pragma once
// thread_owner.h — RAII thread ownership wrapper
// Ensures threads are properly joined on destruction

#include <thread>
#include <functional>
#include <atomic>
#include <string>
#include "logging.h"

namespace sr {

class ThreadOwner {
public:
    ThreadOwner() = default;

    explicit ThreadOwner(std::wstring name)
        : name_(std::move(name)) {}

    ~ThreadOwner() {
        stop();
    }

    // Start the thread with a callable
    template <typename Func, typename... Args>
    void start(Func&& func, Args&&... args) {
        if (thread_.joinable()) {
            SR_LOG_WARN(L"ThreadOwner '%s': already running, stopping first", name_.c_str());
            stop();
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(std::forward<Func>(func), std::forward<Args>(args)...);
        SR_LOG_INFO(L"ThreadOwner '%s': started (id=%u)", name_.c_str(),
                    static_cast<unsigned>(std::hash<std::thread::id>{}(thread_.get_id()) & 0xFFFF));
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            SR_LOG_INFO(L"ThreadOwner '%s': joining...", name_.c_str());
            thread_.join();
            SR_LOG_INFO(L"ThreadOwner '%s': joined", name_.c_str());
        }
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    const std::wstring& name() const { return name_; }

    // Non-copyable, moveable
    ThreadOwner(const ThreadOwner&) = delete;
    ThreadOwner& operator=(const ThreadOwner&) = delete;
    ThreadOwner(ThreadOwner&& other) noexcept
        : name_(std::move(other.name_))
        , running_(other.running_.load())
        , thread_(std::move(other.thread_)) {
        other.running_.store(false);
    }
    ThreadOwner& operator=(ThreadOwner&& other) noexcept {
        if (this != &other) {
            stop();
            name_ = std::move(other.name_);
            running_.store(other.running_.load());
            thread_ = std::move(other.thread_);
            other.running_.store(false);
        }
        return *this;
    }

private:
    std::wstring       name_ = L"unnamed";
    std::atomic<bool>  running_{false};
    std::thread        thread_;
};

} // namespace sr
