#pragma once

#include <chrono>
#include <optional>

namespace sr::fedora {

class RecordingClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void start(TimePoint now) {
        started_at_ = now;
        paused_at_.reset();
        paused_for_ = Clock::duration::zero();
    }

    void pause(TimePoint now) {
        if (started_at_ && !paused_at_) paused_at_ = now;
    }

    void resume(TimePoint now) {
        if (!paused_at_) return;
        paused_for_ += now - *paused_at_;
        paused_at_.reset();
    }

    [[nodiscard]] bool paused() const { return paused_at_.has_value(); }

    [[nodiscard]] Clock::duration elapsed(TimePoint now) const {
        if (!started_at_) return Clock::duration::zero();
        const auto end = paused_at_.value_or(now);
        return end - *started_at_ - paused_for_;
    }

private:
    std::optional<TimePoint> started_at_;
    std::optional<TimePoint> paused_at_;
    Clock::duration paused_for_{};
};

}  // namespace sr::fedora
