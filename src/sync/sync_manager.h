#pragma once
// sync_manager.h — A/V presentation timestamp (PTS) alignment using QPC clock
// T014: Converts QPC timestamps to 100ns units, tracks pause offsets for PTS rebasing

#include <windows.h>
#include <cstdint>

namespace sr {

class SyncManager {
public:
    SyncManager() = default;

    // Call at recording start; anchors the QPC base time
    void start() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        anchor_qpc_       = now.QuadPart;
        paused_accum_100ns_ = 0;
        pause_start_qpc_  = 0;
        freq_ = get_freq();
    }

    // Call immediately at pause
    void pause() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        pause_start_qpc_ = now.QuadPart;
    }

    // Call immediately at resume; accumulates pause duration
    void resume() {
        if (pause_start_qpc_ > 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            int64_t paused_ticks = now.QuadPart - pause_start_qpc_;
            paused_accum_100ns_ += ticks_to_100ns(paused_ticks);
            pause_start_qpc_ = 0;
        }
    }

    // Convert any QPC tick value to a rebased PTS (100ns units)
    // Suitable for both video frames and audio packets
    int64_t to_pts(int64_t qpc_ticks) const {
        int64_t raw = ticks_to_100ns(qpc_ticks - anchor_qpc_);
        return raw - paused_accum_100ns_;
    }

    // Convenience: PTS at "now"
    int64_t now_pts() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return to_pts(now.QuadPart);
    }

    // Total paused duration in 100ns units
    int64_t paused_total_100ns() const { return paused_accum_100ns_; }

    // QPC ticks at anchor (recording start)
    int64_t anchor_qpc() const { return anchor_qpc_; }

private:
    int64_t anchor_qpc_        = 0;
    int64_t paused_accum_100ns_ = 0;
    int64_t pause_start_qpc_   = 0;
    mutable int64_t freq_      = 0;

    static int64_t get_freq() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }

    int64_t ticks_to_100ns(int64_t ticks) const {
        if (freq_ == 0) {
            const_cast<SyncManager*>(this)->freq_ = get_freq();
        }
        // Avoid overflow: use double arithmetic (sufficient precision for 60-min sessions)
        return static_cast<int64_t>(
            static_cast<double>(ticks) * 10'000'000.0 /
            static_cast<double>(freq_)
        );
    }
};

} // namespace sr
