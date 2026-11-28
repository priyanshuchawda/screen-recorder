#pragma once
// QPC-backed session timeline shared by captured video and WASAPI audio.

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sr {

enum class TimestampMappingStatus {
    Mapped,
    BeforeSession,
    OverlapsPause
};

class SyncManager {
public:
    SyncManager() : freq_(get_freq()) {}

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        anchor_qpc_ = now.QuadPart;
        anchor_100ns_ = ticks_to_100ns(anchor_qpc_);
        paused_accum_100ns_ = 0;
        pause_start_100ns_ = 0;
        pause_intervals_.clear();
    }

    void pause() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        pause_at_qpc(now.QuadPart);
    }

    void pause_at_qpc(int64_t qpc_ticks) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (anchor_qpc_ == 0 || pause_start_100ns_ != 0) return;
        pause_start_100ns_ = ticks_to_100ns(qpc_ticks);
        pause_intervals_.push_back({ pause_start_100ns_, 0 });
    }

    void resume() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        resume_at_qpc(now.QuadPart);
    }

    void resume_at_qpc(int64_t qpc_ticks) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pause_start_100ns_ == 0) return;
        const int64_t end = ticks_to_100ns(qpc_ticks);
        const int64_t duration = (end > pause_start_100ns_) ? end - pause_start_100ns_ : 0;
        pause_intervals_.back().end_100ns = end;
        paused_accum_100ns_ += duration;
        pause_start_100ns_ = 0;
    }

    // A frame captured during a pause is rejected. Timestamps from before a
    // pause remain mapped with only pauses that precede that timestamp removed.
    bool try_map_qpc_ticks(int64_t qpc_ticks, int64_t& pts_100ns) const {
        return map_qpc_ticks(qpc_ticks, pts_100ns) == TimestampMappingStatus::Mapped;
    }

    TimestampMappingStatus map_qpc_ticks(int64_t qpc_ticks, int64_t& pts_100ns) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_absolute_100ns_locked(ticks_to_100ns(qpc_ticks), 0, pts_100ns);
    }

    // WASAPI supplies its QPC position in 100ns units. Reject an entire packet
    // if any part of its capture interval overlaps a user pause.
    bool try_map_qpc_100ns(int64_t absolute_qpc_100ns,
                           int64_t duration_100ns,
                           int64_t& pts_100ns) const {
        return map_qpc_100ns(absolute_qpc_100ns, duration_100ns, pts_100ns) ==
               TimestampMappingStatus::Mapped;
    }

    TimestampMappingStatus map_qpc_100ns(int64_t absolute_qpc_100ns,
                                         int64_t duration_100ns,
                                         int64_t& pts_100ns) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_absolute_100ns_locked(absolute_qpc_100ns,
                                         (duration_100ns > 0) ? duration_100ns : 0,
                                         pts_100ns);
    }

    int64_t to_pts(int64_t qpc_ticks) const {
        int64_t pts = 0;
        if (try_map_qpc_ticks(qpc_ticks, pts)) return pts;
        std::lock_guard<std::mutex> lock(mutex_);
        // During pause the public session clock freezes at its boundary.
        return (pause_start_100ns_ > anchor_100ns_)
            ? pause_start_100ns_ - anchor_100ns_ - paused_accum_100ns_
            : 0;
    }

    int64_t now_pts() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return to_pts(now.QuadPart);
    }

    int64_t paused_total_100ns() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return paused_accum_100ns_;
    }

    bool is_paused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pause_start_100ns_ != 0;
    }

    int64_t anchor_qpc() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return anchor_qpc_;
    }
    int64_t anchor_100ns() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return anchor_100ns_;
    }

private:
    struct PauseInterval { int64_t start_100ns; int64_t end_100ns; };
    int64_t anchor_qpc_ = 0;
    int64_t anchor_100ns_ = 0;
    int64_t paused_accum_100ns_ = 0;
    int64_t pause_start_100ns_ = 0;
    int64_t freq_ = 0;
    std::vector<PauseInterval> pause_intervals_;
    mutable std::mutex mutex_;

    TimestampMappingStatus map_absolute_100ns_locked(int64_t absolute_100ns,
                                                      int64_t duration_100ns,
                                                      int64_t& pts_100ns) const {
        if (anchor_qpc_ == 0 || absolute_100ns < anchor_100ns_)
            return TimestampMappingStatus::BeforeSession;
        int64_t removed_pause = 0;
        const int64_t packet_end = absolute_100ns + duration_100ns;
        for (const auto& interval : pause_intervals_) {
            const int64_t end = interval.end_100ns ? interval.end_100ns : INT64_MAX;
            if (duration_100ns == 0) {
                if (absolute_100ns >= interval.start_100ns && absolute_100ns < end)
                    return TimestampMappingStatus::OverlapsPause;
            } else if (absolute_100ns < end && packet_end > interval.start_100ns) {
                return TimestampMappingStatus::OverlapsPause;
            }
            if (interval.end_100ns != 0 && absolute_100ns >= interval.end_100ns) {
                removed_pause += interval.end_100ns - interval.start_100ns;
            }
        }
        pts_100ns = absolute_100ns - anchor_100ns_ - removed_pause;
        return TimestampMappingStatus::Mapped;
    }

    static int64_t get_freq() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }

    int64_t ticks_to_100ns(int64_t ticks) const {
        return static_cast<int64_t>(static_cast<double>(ticks) * 10'000'000.0 /
                                    static_cast<double>(freq_));
    }
};

} // namespace sr
