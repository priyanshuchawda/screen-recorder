#pragma once

#include <mutex>

namespace sr {

class StopFlow {
public:
    bool begin_stop(bool exit_after_stop) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_in_progress_) {
            exit_after_stop_ = exit_after_stop_ || exit_after_stop;
            return false;
        }

        stop_in_progress_ = true;
        exit_after_stop_ = exit_after_stop;
        return true;
    }

    bool complete_stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool should_exit = exit_after_stop_;
        stop_in_progress_ = false;
        exit_after_stop_ = false;
        return should_exit;
    }

    bool stop_in_progress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_in_progress_;
    }

    bool exit_requested_after_stop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return exit_after_stop_;
    }

private:
    mutable std::mutex mutex_;
    bool stop_in_progress_ = false;
    bool exit_after_stop_ = false;
};

} // namespace sr
