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
    explicit FixedFramePacer(uint32_t fps = 30) { set_fps(fps); }

    void set_fps(uint32_t fps) {
        fps_ = (std::max)(1u, fps);
        reset();
        repeats_ = 0;
        skipped_ = 0;
    }

    void begin(int64_t first_pts) {
        started_ = true;
        origin_pts_ = first_pts;
        next_slot_ = 0;
    }

    void reset() {
        started_ = false;
        origin_pts_ = 0;
        next_slot_ = 0;
    }

    bool started() const { return started_; }
    int64_t next_deadline() const {
        return started_ ? deadline_for_slot(next_slot_) : (std::numeric_limits<int64_t>::max)();
    }
    bool output_due(int64_t now_pts) const {
        return started_ && now_pts >= next_deadline();
    }

    // Returns -1 when no output is due. If several slots elapsed, emits only
    // the latest due slot and counts the earlier slots as skipped (no burst).
    int64_t emit_due(int64_t now_pts, bool has_new_frame) {
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
        if (due_slot > next_slot_) {
            skipped_ += due_slot - next_slot_;
            next_slot_ = due_slot;
        }

        return emit_next(has_new_frame);
    }

    // Drain one newest captured image during stop if it arrived before its
    // next nominal slot. This prevents it being silently left in the queue.
    int64_t emit_next(bool has_new_frame) {
        if (!started_) return -1;
        const int64_t result = deadline_for_slot(next_slot_);
        if (!has_new_frame) ++repeats_;
        ++next_slot_;
        return result;
    }

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
};

} // namespace sr
