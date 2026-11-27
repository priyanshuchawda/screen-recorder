#pragma once

#include <cstddef>
#include <deque>
#include <algorithm>
#include <utility>

namespace sr {

enum class DeferredSubmitResult {
    Accepted,
    NotAccepted,
    Failed
};

struct DeferredSubmitSummary {
    size_t accepted = 0;
    size_t not_accepted = 0;
    size_t failed = 0;
};

template <typename T, size_t Capacity>
class DeferredInputQueue {
    static_assert(Capacity > 0);

public:
    bool try_push(T value) {
        if (items_.size() >= Capacity) return false;
        try {
            items_.push_back(std::move(value));
        } catch (...) {
            return false;
        }
        if (items_.size() > high_watermark_) high_watermark_ = items_.size();
        return true;
    }

    bool empty() const noexcept { return items_.empty(); }
    size_t size() const noexcept { return items_.size(); }
    size_t high_watermark() const noexcept { return high_watermark_; }
    T& front() noexcept { return items_.front(); }
    void pop_front() { items_.pop_front(); }
    void clear() noexcept { items_.clear(); }
    void reset() noexcept { items_.clear(); high_watermark_ = 0; }

    template <typename Predicate>
    T* find_if(Predicate&& predicate) {
        const auto found = std::find_if(items_.begin(), items_.end(), predicate);
        return found == items_.end() ? nullptr : &*found;
    }

    template <typename Predicate>
    bool erase_first(Predicate&& predicate) {
        const auto found = std::find_if(items_.begin(), items_.end(), predicate);
        if (found == items_.end()) return false;
        items_.erase(found);
        return true;
    }

private:
    std::deque<T> items_;
    size_t high_watermark_ = 0;
};

template <typename Queue, typename Submit>
DeferredSubmitSummary submit_ready_inputs(Queue& queue,
                                          unsigned& input_events,
                                          Submit&& submit) {
    DeferredSubmitSummary summary{};
    while (input_events > 0 && !queue.empty()) {
        const DeferredSubmitResult result = submit(queue.front());
        --input_events;
        if (result == DeferredSubmitResult::Accepted) {
            queue.pop_front();
            ++summary.accepted;
        } else if (result == DeferredSubmitResult::NotAccepted) {
            ++summary.not_accepted;
            break; // Keep the sample until the next input event.
        } else {
            ++summary.failed;
            break; // The owner decides whether to retry, fallback, or abandon.
        }
    }
    return summary;
}

} // namespace sr
