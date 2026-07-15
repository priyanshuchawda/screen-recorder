#include "recording_clock.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(RecordingClock, SubSecondPauseIsExcludedFromElapsedTime) {
    sr::fedora::RecordingClock clock;
    const auto start = sr::fedora::RecordingClock::TimePoint{};
    clock.start(start);
    clock.pause(start + 400ms);
    clock.resume(start + 900ms);
    EXPECT_EQ(clock.elapsed(start + 1300ms), 800ms);
}

TEST(RecordingClock, MultipleShortPausesAccumulatePrecisely) {
    sr::fedora::RecordingClock clock;
    const auto start = sr::fedora::RecordingClock::TimePoint{};
    clock.start(start);
    clock.pause(start + 100ms);
    clock.resume(start + 350ms);
    clock.pause(start + 500ms);
    clock.resume(start + 900ms);
    EXPECT_EQ(clock.elapsed(start + 1200ms), 550ms);
}

TEST(RecordingClock, ElapsedTimeFreezesWhilePaused) {
    sr::fedora::RecordingClock clock;
    const auto start = sr::fedora::RecordingClock::TimePoint{};
    clock.start(start);
    clock.pause(start + 750ms);
    EXPECT_TRUE(clock.paused());
    EXPECT_EQ(clock.elapsed(start + 4s), 750ms);
}
