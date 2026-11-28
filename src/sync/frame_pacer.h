#pragma once
// frame_pacer.h — authoritative fixed-frame-rate output schedule.

#include <cstdint>
#include <algorithm>
#include <limits>

namespace sr {

// Fixed-rate output clock. Capture arrivals only replace the pending image;
// this clock is the sole authority that decides when a video sample is emitted.
class FixedFramePacer {
public:
    struct SkippedSlots {
        uint64_t count = 0;
        int64_t first_deadline_pts = 0;
        int64_t last_deadline_pts = 0;
    };

    explicit FixedFramePacer(uint32_t fps = 30) { set_fps(fps); }

    void set_fps(uint32_t fps) {
        fps_ = (std::max)(1u, fps);
        reset();
        repeats_ = 0;
        skipped_ = 0;
        last_skipped_ = {};
    }

    void begin(int64_t first_pts) {
        started_ = true;
        origin_pts_ = first_pts;
        next_slot_ = 0;
        startup_bootstrap_active_ = false;
        startup_fill_frames_ = 0;
        last_skipped_ = {};
    }

    // Initial startup may finish after several CFR deadlines. Backfill those
    // slots with the first usable image so mux timestamps remain evenly spaced.
    // This grace is explicit and is never enabled by pause/resume reanchoring.
    void begin_startup(int64_t first_pts) {
        begin(first_pts);
        startup_bootstrap_active_ = true;
    }

    void reset() {
        started_ = false;
        origin_pts_ = 0;
        next_slot_ = 0;
        startup_bootstrap_active_ = false;
        startup_fill_frames_ = 0;
        last_skipped_ = {};
    }

    bool started() const { return started_; }
    bool startup_bootstrap_active() const { return startup_bootstrap_active_; }
    uint64_t startup_fill_frames() const { return startup_fill_frames_; }
    int64_t next_deadline() const {
        return started_ ? deadline_for_slot(next_slot_) : (std::numeric_limits<int64_t>::max)();
    }
    bool output_due(int64_t now_pts) const {
        return started_ && now_pts >= next_deadline();
    }

    // Returns -1 when no output is due. During the explicit initial bootstrap,
    // each elapsed CFR slot is emitted in order using the opening image. After
    // bootstrap, emits only the latest due slot and counts earlier slots.
    int64_t emit_due(int64_t now_pts, bool has_new_frame) {
        last_skipped_ = {};
        if (!output_due(now_pts)) return -1;

        const int64_t elapsed = now_pts - origin_pts_;
        const uint64_t whole_seconds = static_cast<uint64_t>(elapsed / kTicksPerSecond);
        const uint64_t remainder = static_cast<uint64_t>(elapsed % kTicksPerSecond);
        // Deadlines use floor(slot * ticks / fps). Invert that exact rule:
        // the greatest slot whose floored deadline is <= elapsed is
        // floor((((elapsed + 1) * fps) - 1) / ticks). The split form avoids
        // multiplying the full elapsed timestamp and handles 30-FPS rounding.
        const uint64_t due_slot = whole_seconds * fps_ +
            (remainder * fps_ + fps_ - 1) / kTicksPerSecond;

        if (startup_bootstrap_active_) {
            if (next_slot_ > 0) ++startup_fill_frames_;
            const int64_t output_pts = emit_current_slot(has_new_frame);
            // Stop backfilling as soon as the next slot is in the future.
            // Misses after that point use the normal strict accounting path.
            startup_bootstrap_active_ = output_due(now_pts);
            return output_pts;
        }

        if (due_slot > next_slot_) {
            last_skipped_.count = due_slot - next_slot_;
            last_skipped_.first_deadline_pts = deadline_for_slot(next_slot_);
            last_skipped_.last_deadline_pts = deadline_for_slot(due_slot - 1);
            skipped_ += last_skipped_.count;
            next_slot_ = due_slot;
        }

        return emit_current_slot(has_new_frame);
    }

    // Drain one newest captured image during stop if it arrived before its
    // next nominal slot. This prevents it being silently left in the queue.
    int64_t emit_next(bool has_new_frame) {
        last_skipped_ = {};
        const int64_t output_pts = emit_current_slot(has_new_frame);
        startup_bootstrap_active_ = false;
        return output_pts;
    }

    const SkippedSlots& last_skipped_slots() const { return last_skipped_; }

private:
    int64_t emit_current_slot(bool has_new_frame) {
        if (!started_) return -1;
        const int64_t result = deadline_for_slot(next_slot_);
        if (!has_new_frame) ++repeats_;
        ++next_slot_;
        return result;
    }

public:

    uint64_t emitted_repeats() const { return repeats_; }
    uint64_t skipped_slots() const { return skipped_; }
    uint32_t fps() const { return fps_; }

private:
    int64_t deadline_for_slot(uint64_t slot) const {
        return origin_pts_ + static_cast<int64_t>(slot * kTicksPerSecond / fps_);
    }

    static constexpr uint64_t kTicksPerSecond = 10'000'000;
    uint32_t fps_ = 30;
    bool started_ = false;
    int64_t origin_pts_ = 0;
    uint64_t next_slot_ = 0;
    uint64_t repeats_ = 0;
    uint64_t skipped_ = 0;
    bool startup_bootstrap_active_ = false;
    uint64_t startup_fill_frames_ = 0;
    SkippedSlots last_skipped_{};
};

} // namespace sr
