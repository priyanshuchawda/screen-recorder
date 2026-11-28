#pragma once

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "encoder/async_mft_limits.h"

namespace sr {

// Tracks a bounded no-progress window without carrying stale idle time into a
// later recording burst. A missing event after input is queued expires this
// deadline; ordinary idle waits remain indefinite.
class AsyncMftStallWatchdog {
public:
    using Clock = std::chrono::steady_clock;

    explicit AsyncMftStallWatchdog(std::chrono::milliseconds timeout) : timeout_(timeout) {}

    std::optional<std::chrono::milliseconds> until_stalled(bool work_pending, Clock::time_point now) {
        if (!work_pending) {
            active_ = false;
            return std::nullopt;
        }
        if (!active_) {
            active_ = true;
            deadline_ = now + timeout_;
        }
        if (now >= deadline_) return std::chrono::milliseconds::zero();
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
    }

    void mark_progress(Clock::time_point now) {
        active_ = true;
        deadline_ = now + timeout_;
    }

private:
    std::chrono::milliseconds timeout_;
    Clock::time_point deadline_{};
    bool active_ = false;
};

// Thread-safe bounded coordination state for an asynchronous Media Foundation
// transform. The encoder worker remains the sole owner of IMFTransform calls;
// event callbacks only grant credits or signal terminal state here.
template <typename Input, typename Output>
class AsyncMftScheduler {
public:
    struct InputRequest {
        DWORD stream_id = 0;
        Input input;
    };

    explicit AsyncMftScheduler(std::size_t input_capacity = kAsyncMftInputQueueCapacity,
                               std::size_t max_in_flight = kAsyncMftMaxInFlightInputs,
                               std::size_t output_capacity = kAsyncMftOutputQueueCapacity,
                               std::size_t output_byte_capacity = kAsyncMftOutputQueueByteCapacity)
        : input_capacity_(input_capacity), max_in_flight_(max_in_flight),
          output_capacity_(output_capacity), output_byte_capacity_(output_byte_capacity) {}

    void reset() {
        std::lock_guard lock(mutex_);
        inputs_.clear();
        input_credits_.clear();
        outputs_.clear();
        output_bytes_ = output_high_water_ = output_bytes_high_water_ = 0;
        in_flight_ = output_credits_ = output_credit_high_water_ = input_credit_high_water_ = 0;
        input_queue_high_water_ = 0;
        need_input_events_received_ = have_output_events_received_ = 0;
        failure_reason_ = S_OK;
        request_outstanding_ = drain_requested_ = drain_command_sent_ = false;
        drain_complete_ = failed_ = stopped_ = false;
        worker_drained_ = false;
        input_capacity_waiter_ = false;
        cv_work_.notify_all();
        cv_output_.notify_all();
    }

    bool enqueue_input(Input input) {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_ || drain_requested_ ||
            inputs_.size() + (request_outstanding_ ? 1u : 0u) >= input_capacity_) return false;
        try {
            inputs_.push_back(std::move(input));
        } catch (...) {
            fail_locked(E_OUTOFMEMORY);
            return false;
        }
        input_queue_high_water_ = (std::max)(input_queue_high_water_, inputs_.size());
        cv_work_.notify_one();
        return true;
    }

    // Producer capacity is queried non-blockingly by the controller. The
    // controller owns the pending frame and keeps servicing audio/output while
    // capacity is unavailable.
    bool input_capacity_available() const {
        std::lock_guard lock(mutex_);
        return input_capacity_available_locked();
    }

    // Returns true when capacity opened before registration; otherwise a
    // future accepted input will request a producer wake if it opens a slot.
    bool register_input_capacity_waiter() {
        std::lock_guard lock(mutex_);
        if (input_capacity_available_locked()) return true;
        input_capacity_waiter_ = true;
        return false;
    }

    void grant_input(DWORD stream_id) {
        std::lock_guard lock(mutex_);
        // Existing application inputs must still consume NeedInput credits
        // after drain is requested. The drain command is sent only after that
        // queue has been accepted by the MFT.
        if (stopped_ || failed_ || drain_command_sent_ || drain_complete_) return;
        if (input_credits_.size() >= kMaxQueuedEvents) {
            fail_locked(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            return;
        }
        ++need_input_events_received_;
        input_credit_high_water_ = (std::max)(input_credit_high_water_, input_credits_.size() + 1);
        try {
            input_credits_.push_back(stream_id);
        } catch (...) {
            fail_locked(E_OUTOFMEMORY);
            return;
        }
        cv_work_.notify_one();
    }

    void grant_output() {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_ || drain_complete_) return;
        if (output_credits_ >= kMaxQueuedEvents) {
            fail_locked(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            return;
        }
        ++have_output_events_received_;
        ++output_credits_;
        if (output_credits_ > output_credit_high_water_) {
            output_credit_high_water_ = output_credits_;
        }
        cv_work_.notify_one();
    }

    bool output_ready() const {
        std::lock_guard lock(mutex_);
        return output_credits_ != 0 && !stopped_ && !failed_;
    }

    bool take_output_event() {
        std::lock_guard lock(mutex_);
        if (output_credits_ == 0 || stopped_ || failed_) return false;
        --output_credits_;
        return true;
    }

    void restore_output_event() {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_) return;
        if (output_credits_ >= kMaxQueuedEvents) {
            fail_locked(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            return;
        }
        ++output_credits_;
        cv_work_.notify_one();
    }

    std::size_t output_events_pending() const {
        std::lock_guard lock(mutex_);
        return output_credits_;
    }
    std::size_t input_count() const {
        std::lock_guard lock(mutex_);
        return inputs_.size();
    }
    std::size_t abandon_queued_inputs() {
        std::lock_guard lock(mutex_);
        const std::size_t count = inputs_.size();
        inputs_.clear();
        input_credits_.clear();
        cv_work_.notify_all();
        return count;
    }
    std::size_t input_credit_high_water() const {
        std::lock_guard lock(mutex_);
        return input_credit_high_water_;
    }
    std::size_t input_queue_high_water() const {
        std::lock_guard lock(mutex_);
        return input_queue_high_water_;
    }
    uint64_t need_input_events_received() const {
        std::lock_guard lock(mutex_);
        return need_input_events_received_;
    }
    uint64_t have_output_events_received() const {
        std::lock_guard lock(mutex_);
        return have_output_events_received_;
    }

    bool output_has_capacity() const {
        std::lock_guard lock(mutex_);
        return !stopped_ && !failed_ && outputs_.size() < output_capacity_ &&
               output_bytes_ < output_byte_capacity_;
    }

    bool input_ready() const {
        std::lock_guard lock(mutex_);
        return !stopped_ && !failed_ && !drain_command_sent_ && !request_outstanding_ &&
               in_flight_ < max_in_flight_ && !inputs_.empty() && !input_credits_.empty();
    }

    std::optional<InputRequest> take_input_request() {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_ || drain_command_sent_ || request_outstanding_ ||
            in_flight_ >= max_in_flight_ || inputs_.empty() || input_credits_.empty()) return std::nullopt;
        const DWORD stream_id = input_credits_.front();
        input_credits_.pop_front();
        Input input = std::move(inputs_.front());
        inputs_.pop_front();
        request_outstanding_ = true;
        cv_work_.notify_one();
        return InputRequest{stream_id, std::move(input)};
    }

    // Call after ProcessInput returns S_OK. On MF_E_NOTACCEPTING, return the
    // exact sample with retry_input_front(); a future NeedInput grants a new credit.
    bool mark_input_accepted() {
        std::lock_guard lock(mutex_);
        if (!request_outstanding_) return false;
        request_outstanding_ = false;
        ++in_flight_;
        cv_work_.notify_one();
        const bool wake_producer = input_capacity_waiter_ && input_capacity_available_locked();
        if (wake_producer) input_capacity_waiter_ = false;
        return wake_producer;
    }

    bool retry_input_front(Input input) {
        std::lock_guard lock(mutex_);
        if (!request_outstanding_) return false;
        request_outstanding_ = false;
        if (stopped_ || failed_) return false;
        if (inputs_.size() >= input_capacity_) {
            fail_locked(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            return false;
        }
        try {
            inputs_.push_front(std::move(input));
        } catch (...) {
            fail_locked(E_OUTOFMEMORY);
            return false;
        }
        cv_work_.notify_one();
        return true;
    }

    void mark_output_produced() {
        std::lock_guard lock(mutex_);
        if (in_flight_ > 0) --in_flight_;
        cv_work_.notify_one();
    }

    bool publish_output(Output& output, std::size_t bytes) {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_) return false;
        if (bytes > output_byte_capacity_) {
            fail_locked(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
            return false;
        }
        if (outputs_.size() >= output_capacity_ ||
            bytes > output_byte_capacity_ - output_bytes_) return false;
        try {
            outputs_.emplace_back(std::move(output), bytes);
        } catch (...) {
            fail_locked(E_OUTOFMEMORY);
            return false;
        }
        output_bytes_ += bytes;
        if (outputs_.size() > output_high_water_) output_high_water_ = outputs_.size();
        if (output_bytes_ > output_bytes_high_water_) output_bytes_high_water_ = output_bytes_;
        cv_output_.notify_one();
        return true;
    }

    std::optional<Output> try_pop_output() {
        std::lock_guard lock(mutex_);
        if (outputs_.empty()) return std::nullopt;
        auto entry = std::move(outputs_.front());
        outputs_.pop_front();
        output_bytes_ -= entry.second;
        cv_work_.notify_all();
        return std::move(entry.first);
    }

    std::size_t discard_outputs() {
        std::deque<std::pair<Output, std::size_t>> discarded;
        {
            std::lock_guard lock(mutex_);
            discarded.swap(outputs_);
            output_bytes_ = 0;
        }
        cv_work_.notify_all();
        return discarded.size();
    }

    std::size_t output_count() const {
        std::lock_guard lock(mutex_);
        return outputs_.size();
    }
    std::size_t output_bytes() const {
        std::lock_guard lock(mutex_);
        return output_bytes_;
    }
    std::size_t output_high_water() const {
        std::lock_guard lock(mutex_);
        return output_high_water_;
    }
    std::size_t output_bytes_high_water() const {
        std::lock_guard lock(mutex_);
        return output_bytes_high_water_;
    }
    std::size_t in_flight_inputs() const {
        std::lock_guard lock(mutex_);
        return in_flight_;
    }

    void request_drain() {
        std::lock_guard lock(mutex_);
        if (stopped_ || failed_ || drain_complete_) return;
        drain_requested_ = true;
        cv_work_.notify_all();
    }

    bool take_drain_command() {
        std::lock_guard lock(mutex_);
        if (!drain_requested_ || drain_command_sent_ || !inputs_.empty() ||
            request_outstanding_ || stopped_ || failed_) return false;
        drain_command_sent_ = true;
        input_credits_.clear();
        return true;
    }

    void signal_drain_complete() {
        std::lock_guard lock(mutex_);
        drain_complete_ = true;
        cv_work_.notify_all();
        cv_output_.notify_all();
    }
    bool drain_complete() const {
        std::lock_guard lock(mutex_);
        return drain_complete_;
    }
    void signal_worker_drained() {
        std::lock_guard lock(mutex_);
        worker_drained_ = true;
        cv_output_.notify_all();
    }
    bool worker_drained() const {
        std::lock_guard lock(mutex_);
        return worker_drained_;
    }

    void fail(HRESULT reason) {
        std::lock_guard lock(mutex_);
        fail_locked(reason);
    }
    bool failed() const {
        std::lock_guard lock(mutex_);
        return failed_;
    }
    HRESULT failure_reason() const {
        std::lock_guard lock(mutex_);
        return failure_reason_;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        input_credits_.clear();
        cv_work_.notify_all();
        cv_output_.notify_all();
    }
    bool stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }
    bool wait_for_work(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_work_.wait_for(lock, timeout, [&] {
            return stopped_ || failed_ || (drain_complete_ && output_credits_ == 0) ||
                   (output_credits_ != 0 && outputs_.size() < output_capacity_ &&
                    output_bytes_ < output_byte_capacity_) ||
                   (!request_outstanding_ && in_flight_ < max_in_flight_ &&
                    !inputs_.empty() && !input_credits_.empty()) ||
                   (drain_requested_ && !drain_command_sent_ && inputs_.empty() &&
                    !request_outstanding_);
        });
    }

    void wait_for_work() {
        std::unique_lock lock(mutex_);
        cv_work_.wait(lock, [&] {
            return stopped_ || failed_ || (drain_complete_ && output_credits_ == 0) ||
                   !inputs_.empty() ||
                   (output_credits_ != 0 && outputs_.size() < output_capacity_ &&
                    output_bytes_ < output_byte_capacity_) ||
                   (!request_outstanding_ && in_flight_ < max_in_flight_ &&
                    !inputs_.empty() && !input_credits_.empty()) ||
                   (drain_requested_ && !drain_command_sent_ && inputs_.empty() &&
                    !request_outstanding_);
        });
    }

    bool wait_for_output_space(std::size_t bytes, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_work_.wait_for(lock, timeout, [&] {
            return stopped_ || failed_ ||
                   (bytes <= output_byte_capacity_ && outputs_.size() < output_capacity_ &&
                    bytes <= output_byte_capacity_ - output_bytes_);
        });
    }

    void wait_for_output_space(std::size_t bytes) {
        std::unique_lock lock(mutex_);
        cv_work_.wait(lock, [&] {
            return stopped_ || failed_ ||
                   (bytes <= output_byte_capacity_ && outputs_.size() < output_capacity_ &&
                    bytes <= output_byte_capacity_ - output_bytes_);
        });
    }

    bool wait_for_output_or_terminal(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_output_.wait_for(lock, timeout, [&] {
            return stopped_ || failed_ || worker_drained_ || !outputs_.empty();
        });
    }

private:
    static constexpr std::size_t kMaxQueuedEvents = 16;

    bool input_capacity_available_locked() const {
        return !stopped_ && !failed_ && !drain_requested_ &&
               inputs_.size() + (request_outstanding_ ? 1u : 0u) < input_capacity_;
    }

    void fail_locked(HRESULT reason) {
        failed_ = true;
        failure_reason_ = reason;
        cv_work_.notify_all();
        cv_output_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_work_;
    std::condition_variable cv_output_;
    std::deque<Input> inputs_;
    std::deque<DWORD> input_credits_;
    std::deque<std::pair<Output, std::size_t>> outputs_;
    std::size_t input_capacity_;
    std::size_t max_in_flight_;
    std::size_t output_capacity_;
    std::size_t output_byte_capacity_;
    std::size_t output_bytes_ = 0;
    std::size_t output_high_water_ = 0;
    std::size_t output_bytes_high_water_ = 0;
    std::size_t in_flight_ = 0;
    std::size_t output_credits_ = 0;
    std::size_t output_credit_high_water_ = 0;
    std::size_t input_credit_high_water_ = 0;
    std::size_t input_queue_high_water_ = 0;
    uint64_t need_input_events_received_ = 0;
    uint64_t have_output_events_received_ = 0;
    HRESULT failure_reason_ = S_OK;
    bool request_outstanding_ = false;
    bool drain_requested_ = false;
    bool drain_command_sent_ = false;
    bool drain_complete_ = false;
    bool worker_drained_ = false;
    bool input_capacity_waiter_ = false;
    bool failed_ = false;
    bool stopped_ = false;
};

} // namespace sr
