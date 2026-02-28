#pragma once
// frame_pacer.h — T038: Frame pacing normalization layer
//
// Absorbs WGC timestamp jitter so the encoder receives smoothly-spaced frames:
//   • Detects gaps > 1.5× target interval → caller should insert a duplicate frame
//   • Clamps large jumps to prevent PTS drift accumulation
//   • On backpressure (queue full) → returns Drop so the caller discards the frame
//   • Tracks duplicate count and drop count as telemetry
//
// Usage (in encode_loop):
//   pacer_.initialize(fps);
//   ...
//   int64_t out_pts;
//   PaceAction action = pacer_.pace_frame(frame.pts, queue_full, &out_pts);
//   if (action == PaceAction::Drop)      { skip; }
//   if (action == PaceAction::Duplicate) { encode last frame again with out_pts; }
//   encode frame using out_pts;

#include <cstdint>
#include <algorithm>
#include "utils/logging.h"

namespace sr {

enum class PaceAction {
    Accept,     // Frame is fine — use *out_pts as the corrected PTS
    Duplicate,  // Gap > 1.5× target — insert a duplicate of the PREVIOUS frame first
    Drop,       // Queue backpressure — caller should discard this frame entirely
};

class FramePacer {
public:
    FramePacer() = default;

    // Call once before recording starts (or on resume after reset()).
    // fps: target frames per second (e.g. 30 or 60)
    void initialize(uint32_t fps) {
        target_interval_100ns_ = (fps > 0)
            ? (10'000'000LL / static_cast<int64_t>(fps))
            : 333'333LL;
        last_pts_     = -1;
        smoothed_pts_ = -1;
        dups_         = 0;
        drops_        = 0;
        SR_LOG_INFO(L"[FramePacer] Initialized: target interval %lld 100ns (~%u fps)",
                    target_interval_100ns_, fps);
    }

    // Call after pause to prevent a gap being mistaken for a frame skip.
    void reset() {
        last_pts_     = -1;
        smoothed_pts_ = -1;
    }

    // Classify the incoming raw_pts and compute a corrected PTS.
    //   queue_full — true when the frame queue is at capacity (backpressure)
    //   *out_pts  — receives the PTS to stamp on the encoded frame
    // Returns PaceAction indicating how the caller should handle this frame.
    PaceAction pace_frame(int64_t raw_pts, bool queue_full, int64_t* out_pts) {
        // Backpressure: drop this frame entirely
        if (queue_full) {
            ++drops_;
            *out_pts = raw_pts;
            return PaceAction::Drop;
        }

        // First frame — bootstrap pacing state
        if (last_pts_ < 0) {
            smoothed_pts_ = raw_pts;
            last_pts_     = raw_pts;
            *out_pts      = raw_pts;
            return PaceAction::Accept;
        }

        int64_t gap             = raw_pts - last_pts_;
        int64_t threshold_150pct = target_interval_100ns_ * 3 / 2;
        bool    need_dup         = (gap > threshold_150pct);

        if (need_dup) {
            ++dups_;
            if (dups_ <= 3 || (dups_ % 30) == 0) {
                SR_LOG_INFO(L"[FramePacer] Gap %lld > 1.5× target %lld — signalling duplicate (count=%u)",
                            gap, target_interval_100ns_, dups_);
            }
        }

        // Clamp PTS advance to avoid compounding drift on multi-frame gaps.
        // We advance by at most 2× the target interval per frame.
        int64_t clamped_gap = std::min(gap, target_interval_100ns_ * 2);
        smoothed_pts_       = smoothed_pts_ + clamped_gap;
        last_pts_           = raw_pts;
        *out_pts            = smoothed_pts_;

        return need_dup ? PaceAction::Duplicate : PaceAction::Accept;
    }

    uint32_t duplicates_inserted() const { return dups_; }
    uint32_t drops()               const { return drops_; }

private:
    int64_t  target_interval_100ns_ = 333'333;
    int64_t  last_pts_              = -1;
    int64_t  smoothed_pts_          = -1;
    uint32_t dups_                  = 0;
    uint32_t drops_                 = 0;
};

} // namespace sr
