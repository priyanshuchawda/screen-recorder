// test_sync_manager.cpp — Unit tests for SyncManager PTS rebasing
// T014 / T019: Validates monotonic PTS output, pause accumulation, and PTS continuity on resume.

#include <gtest/gtest.h>
#include "sync/sync_manager.h"
#include <windows.h>
#include <thread>
#include <chrono>

namespace {

// Helper: get current QPC value
int64_t now_qpc() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// Helper: get QPC frequency
int64_t qpc_freq() {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

// Convert milliseconds to QPC ticks
int64_t ms_to_ticks(int64_t ms) {
    return ms * qpc_freq() / 1000;
}

// Convert 100ns units to milliseconds
int64_t hns_to_ms(int64_t hns) {
    return hns / 10000;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class SyncManagerTest : public ::testing::Test {
protected:
    sr::SyncManager sync_;
};

// T019: after start(), to_pts(anchor) == 0
TEST_F(SyncManagerTest, AnchorPTSIsZero) {
    sync_.start();
    int64_t anchor = sync_.anchor_qpc();
    EXPECT_EQ(sync_.to_pts(anchor), 0);
}

// T019: PTS increases with time
TEST_F(SyncManagerTest, PTSIncreasesWithTime) {
    sync_.start();
    int64_t before = sync_.now_pts();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int64_t after = sync_.now_pts();
    EXPECT_GT(after, before);
}

// T019: PTS is monotonic across multiple calls
TEST_F(SyncManagerTest, PTSIsMonotonic) {
    sync_.start();
    int64_t last_pts = -1;
    for (int i = 0; i < 10; ++i) {
        int64_t pts = sync_.now_pts();
        EXPECT_GE(pts, last_pts) << "PTS went backwards at iteration " << i;
        last_pts = pts;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// T019: paused_total starts at zero after start()
TEST_F(SyncManagerTest, PausedTotalInitiallyZero) {
    sync_.start();
    EXPECT_EQ(sync_.paused_total_100ns(), 0);
}

// T019: After pause+resume, paused_total > 0
TEST_F(SyncManagerTest, PauseAccumulatorGrowsAfterResume) {
    sync_.start();
    EXPECT_EQ(sync_.paused_total_100ns(), 0);

    sync_.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sync_.resume();

    int64_t accum = sync_.paused_total_100ns();
    EXPECT_GT(accum, 0) << "Pause accumulator should be > 0 after 50ms pause";
    // Accumulator should be approximately 50ms = 500000 * 100ns units
    int64_t expected_min = 400'000;   // 40ms lower bound
    int64_t expected_max = 2'000'000; // 200ms upper bound (CI can be slow)
    EXPECT_GE(accum, expected_min);
    EXPECT_LE(accum, expected_max);
}

// T019: PTS output accounts for pause duration (no gap in output PTS)
TEST_F(SyncManagerTest, PTSSubtractsPauseDuration) {
    sync_.start();

    // Simulate recording for 50ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int64_t pts_before_pause = sync_.now_pts();

    // Pause for 100ms
    sync_.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sync_.resume();

    // After resume, PTS should be close to pts_before_pause + small delta
    // (not pts_before_pause + 100ms, because pause duration is subtracted)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    int64_t pts_after_resume = sync_.now_pts();

    // The difference should be ~10ms (the sleep after resume), not ~110ms
    int64_t delta_hns = pts_after_resume - pts_before_pause;
    int64_t delta_ms  = hns_to_ms(delta_hns);

    // Allow generous bounds: should be < 80ms (not ~110ms)
    EXPECT_LT(delta_ms, 80) << "Pause duration should be subtracted from PTS. delta=" << delta_ms << "ms";
    EXPECT_GE(delta_ms, 0)  << "PTS should not go backwards";
}

// T019: Multiple pause/resume cycles accumulate correctly
TEST_F(SyncManagerTest, MultiplePauseCyclesAccumulate) {
    sync_.start();

    int64_t total_expected_pause = 0;

    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sync_.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sync_.resume();
    }

    int64_t accum = sync_.paused_total_100ns();
    // 3 pauses of ~30ms each = ~90ms = ~900000 100ns units
    EXPECT_GT(accum, 500'000) << "3 x 30ms pause should accumulate > 50ms";
    EXPECT_LT(accum, 5'000'000) << "Accumulator should be < 500ms";
}

// T019: Calling resume() without pause() is a no-op (no crash, accum unchanged)
TEST_F(SyncManagerTest, ResumeWithoutPauseIsNoop) {
    sync_.start();
    EXPECT_EQ(sync_.paused_total_100ns(), 0);
    sync_.resume(); // no pause() before this
    EXPECT_EQ(sync_.paused_total_100ns(), 0);
}

// T019: to_pts() with explicit QPC ticks returns correct 100ns PTS
TEST_F(SyncManagerTest, ToPtsConvertsQPCCorrectly) {
    sync_.start();
    // to_pts(anchor) == 0
    EXPECT_EQ(sync_.to_pts(sync_.anchor_qpc()), 0);

    // to_pts(anchor + 1s worth of ticks) == 10000000 (1s in 100ns)
    int64_t one_second_ticks = qpc_freq();
    int64_t pts_1s = sync_.to_pts(sync_.anchor_qpc() + one_second_ticks);
    // Allow ±1% tolerance for floating-point arithmetic
    EXPECT_NEAR(pts_1s, 10'000'000LL, 100'000LL)
        << "1 second should be 10,000,000 in 100ns units";
}

// T019: PTS never goes below zero during pause (before resume is called)
TEST_F(SyncManagerTest, PTSNonNegativeDuringPause) {
    sync_.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sync_.pause();
    // While paused, now_pts() should still reflect time up to pause point (no subtraction until resume)
    int64_t pts_while_paused = sync_.now_pts();
    EXPECT_GE(pts_while_paused, 0) << "PTS should be >= 0 while paused";
}
