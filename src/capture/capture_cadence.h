#pragma once

#include <cstdint>
#include <algorithm>

namespace sr {

// Decides whether a newly arrived WGC frame is due for processing.
class CaptureCadence {
public:
    bool accept(int64_t pts, int64_t interval) {
        interval = (std::max)(int64_t{1}, interval);
        if (next_due_ < 0 || interval != interval_) {
            interval_ = interval;
            next_due_ = pts + interval;
            return true;
        }
        // Permit small delivery jitter without resetting the schedule to the
        // last arrival. Otherwise a slightly early 60 Hz source can lose
        // every other frame at a 60 fps target.
        const int64_t tolerance = interval / 20;
        if (pts < next_due_ - tolerance) return false;
        next_due_ += interval;
        if (next_due_ <= pts) next_due_ = pts + interval;
        return true;
    }

    void reset() { next_due_ = -1; interval_ = 0; }

private:
    int64_t next_due_ = -1;
    int64_t interval_ = 0;
};

} // namespace sr
