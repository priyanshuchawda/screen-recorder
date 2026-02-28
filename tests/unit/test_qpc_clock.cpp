// test_qpc_clock.cpp — Unit tests for QPCClock (T004)

#include <gtest/gtest.h>
#include "utils/qpc_clock.h"
#include <thread>
#include <chrono>

using sr::QPCClock;

TEST(QPCClockTest, FrequencyIsPositive) {
    const auto& clock = QPCClock::instance();
    EXPECT_GT(clock.frequency(), 0);
}

TEST(QPCClockTest, TimestampsAreMonotonic) {
    const auto& clock = QPCClock::instance();
    int64_t prev = clock.now_ns();
    for (int i = 0; i < 100; i++) {
        int64_t curr = clock.now_ns();
        EXPECT_GE(curr, prev);
        prev = curr;
    }
}

TEST(QPCClockTest, NanosecondResolution) {
    const auto& clock = QPCClock::instance();
    // QPC should have at least microsecond resolution; frequency > 1MHz
    EXPECT_GT(clock.frequency(), 1'000'000);
}

TEST(QPCClockTest, HnsConversionWorks) {
    const auto& clock = QPCClock::instance();
    int64_t hns = clock.now_hns();
    EXPECT_GT(hns, 0);
}

TEST(QPCClockTest, MillisecondTimingAccuracy) {
    const auto& clock = QPCClock::instance();
    double start = clock.now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    double end = clock.now_ms();
    double elapsed = end - start;
    // Should be roughly 50ms, allow 20ms tolerance
    EXPECT_GT(elapsed, 30.0);
    EXPECT_LT(elapsed, 100.0);
}

TEST(QPCClockTest, TicksToHnsConsistent) {
    const auto& clock = QPCClock::instance();
    // 1 second worth of ticks should be 10,000,000 HNS
    int64_t one_sec_hns = clock.ticks_to_hns(clock.frequency());
    EXPECT_NEAR(one_sec_hns, 10'000'000, 100); // Allow tiny float rounding
}
