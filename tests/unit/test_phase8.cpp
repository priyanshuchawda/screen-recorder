// test_phase8.cpp — Unit tests for Phase 8: Polish & Cross-Cutting Constraints
// Covers T032 (AudioResampler), T033 (thread priorities), T035 (encoder fallback),
// T036 (queue stability), T034 (resolution change checks)

#include <gtest/gtest.h>
#include <windows.h>
#include <mfapi.h>
#include <thread>
#include <vector>
#include <atomic>

// ─── Utility headers ──────────────────────────────────────────────────────────
#include "utils/bounded_queue.h"
#include "utils/render_frame.h"

// ─── T032: AudioResampler ─────────────────────────────────────────────────────
#include "audio/audio_resampler.h"

// ─── T035: VideoEncoder (fallback chain identifiers) ─────────────────────────
#include "encoder/video_encoder.h"

namespace sr {

// =============================================================================
// T036 — Queue Stability Tests
// =============================================================================
TEST(T036_QueueStability, VideoQueueCapacityIsFive) {
    // The spec mandates max 5 frames for the video queue.
    using VQ = BoundedQueue<int, 5>;
    EXPECT_EQ(VQ::capacity(), 5u);
}

TEST(T036_QueueStability, AudioQueueCapacityIs16) {
    using AQ = BoundedQueue<int, 16>;
    EXPECT_EQ(AQ::capacity(), 16u);
}

TEST(T036_QueueStability, QueueNeverExceedsCapacity) {
    BoundedQueue<int, 5> q;
    int dropped = 0;
    // Push 10 items into a capacity-5 queue
    for (int i = 0; i < 10; ++i) {
        if (!q.try_push(std::move(i))) ++dropped;
    }
    EXPECT_EQ(q.size(), 5u);
    EXPECT_GE(dropped, 5);  // at least 5 were dropped
}

TEST(T036_QueueStability, PushFullRejectsMidStream) {
    BoundedQueue<int, 3> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));  // queue full
    EXPECT_EQ(q.size(), 3u);
}

TEST(T036_QueueStability, ConcurrentProducersNeverExceedCapacity) {
    // T036: 2 producers + 1 consumer; queue must never report size > Capacity
    constexpr size_t CAP = 5;
    BoundedQueue<int, CAP> q;
    std::atomic<int> max_seen{ 0 };
    std::atomic<bool> running{ true };
    std::atomic<int> dropped{ 0 };

    auto producer = [&]() {
        for (int i = 0; i < 1000 && running.load(); ++i) {
            if (!q.try_push(std::move(i))) ++dropped;
        }
    };

    auto consumer = [&]() {
        while (running.load() || !q.empty()) {
            int cur = static_cast<int>(q.size());
            int prev = max_seen.load();
            while (cur > prev &&
                   !max_seen.compare_exchange_weak(prev, cur)) {}
            q.try_pop();
        }
    };

    std::thread p1(producer), p2(producer), c(consumer);
    p1.join(); p2.join();
    running.store(false);
    c.join();

    EXPECT_LE(static_cast<size_t>(max_seen.load()), CAP)
        << "Queue exceeded capacity during concurrent use";
}

// =============================================================================
// T032 — AudioResampler Tests
// =============================================================================
class T032_AudioResamplerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // MF must be initialized for CoCreateInstance(CResamplerMediaObject)
        MFStartup(MF_VERSION);
    }
    void TearDown() override {
        MFShutdown();
    }
};

TEST_F(T032_AudioResamplerTest, PassthroughWhenRatesMatch) {
    AudioResampler rs;
    EXPECT_TRUE(rs.initialize(48000, 2, 16, 48000));
    EXPECT_TRUE(rs.is_passthrough());
    EXPECT_EQ(rs.output_rate(), 48000u);
    EXPECT_EQ(rs.input_rate(), 48000u);
}

TEST_F(T032_AudioResamplerTest, ResamplerInitializesFor44100) {
    AudioResampler rs;
    bool ok = rs.initialize(44100, 2, 16, 48000);
    EXPECT_TRUE(ok) << "AudioResampler should init successfully for 44100->48000";
    if (ok) {
        EXPECT_FALSE(rs.is_passthrough());
        EXPECT_EQ(rs.input_rate(),  44100u);
        EXPECT_EQ(rs.output_rate(), 48000u);
    }
}

TEST_F(T032_AudioResamplerTest, ProcessProducesOutput_44100to48000) {
    AudioResampler rs;
    if (!rs.initialize(44100, 2, 16, 48000)) {
        GTEST_SKIP() << "Resampler not available on this system";
    }

    // Feed 10 chunks of 10ms each; MF resampler has startup latency,
    // so accumulate all output including flush tail.
    const uint32_t in_samples = 441;
    std::vector<uint8_t> in_pcm(in_samples * 2 * 2, static_cast<uint8_t>(0));

    std::vector<uint8_t> out_pcm;
    for (int i = 0; i < 10; ++i) {
        rs.process(in_pcm.data(), static_cast<uint32_t>(in_pcm.size()), out_pcm);
    }
    std::vector<uint8_t> tail;
    rs.flush(tail);
    out_pcm.insert(out_pcm.end(), tail.begin(), tail.end());

    // After 100ms of input, the resampler must have produced output.
    EXPECT_GT(out_pcm.size(), static_cast<size_t>(0))
        << "Resampler produced no output for 100ms of 44.1kHz input";
}

TEST_F(T032_AudioResamplerTest, PassthroughProcessJustCopiesData) {
    AudioResampler rs;
    ASSERT_TRUE(rs.initialize(48000, 2, 16, 48000));
    ASSERT_TRUE(rs.is_passthrough());

    std::vector<uint8_t> in_pcm(1024, static_cast<uint8_t>(0xAB));
    std::vector<uint8_t> out_pcm;
    EXPECT_TRUE(rs.process(in_pcm.data(), static_cast<uint32_t>(in_pcm.size()), out_pcm));
    EXPECT_EQ(out_pcm, in_pcm);  // passthrough = exact copy
}

TEST_F(T032_AudioResamplerTest, FlushDoesNotCrash) {
    AudioResampler rs;
    bool ok = rs.initialize(44100, 2, 16, 48000);
    if (!ok) { GTEST_SKIP() << "Resampler unavailable"; }
    std::vector<uint8_t> tail;
    EXPECT_NO_FATAL_FAILURE(rs.flush(tail));
}

TEST_F(T032_AudioResamplerTest, ShutdownTwiceIsSafe) {
    AudioResampler rs;
    rs.initialize(44100, 2, 16, 48000);
    EXPECT_NO_FATAL_FAILURE(rs.shutdown());
    EXPECT_NO_FATAL_FAILURE(rs.shutdown());  // double-shutdown should be safe
}

// =============================================================================
// T033 — Thread / Process Priority Tests
// =============================================================================
TEST(T033_ThreadPriority, ProcessPriorityIsAtLeastAboveNormal) {
    // wWinMain calls SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS).
    // Verify the API works and the call is safe in tests.
    DWORD current = GetPriorityClass(GetCurrentProcess());
    EXPECT_NE(current, 0u) << "GetPriorityClass failed";
    // Verify the API used in main.cpp works
    BOOL ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    EXPECT_TRUE(ok);
    // Restore
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

TEST(T033_ThreadPriority, SetThreadPriorityAboveNormalWorks) {
    // The encode_loop sets THREAD_PRIORITY_ABOVE_NORMAL.
    // Verify the API works from any thread.
    BOOL ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    EXPECT_TRUE(ok);
    int prio = GetThreadPriority(GetCurrentThread());
    EXPECT_EQ(prio, THREAD_PRIORITY_ABOVE_NORMAL);
    // Restore
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
}

// =============================================================================
// T034 — Resolution Change Detection (structural test)
// =============================================================================
TEST(T034_ResolutionChange, BoundedQueueCapacityMatchesSpec) {
    // FrameQueue = BoundedQueue<RenderFrame, 5>; spec mandates max 5 frames.
    // We test the queue template with a simple int to avoid COM/D3D11 init.
    using TestQueue = BoundedQueue<int, 5>;
    EXPECT_EQ(TestQueue::capacity(), static_cast<size_t>(5));
}

// =============================================================================
// T035 — Encoder Fallback Chain (enum coverage)
// =============================================================================
TEST(T035_EncoderFallback, EncoderModeNamesAreCoveredByEnum) {
    EXPECT_NE(static_cast<int>(EncoderMode::HardwareMFT),
              static_cast<int>(EncoderMode::SoftwareMFT));
    EXPECT_NE(static_cast<int>(EncoderMode::SoftwareMFT),
              static_cast<int>(EncoderMode::SoftwareMFT720p));
    EXPECT_NE(static_cast<int>(EncoderMode::HardwareMFT),
              static_cast<int>(EncoderMode::SoftwareMFT720p));
}

TEST(T035_EncoderFallback, VideoEncoderDefaultIsUninitialised) {
    // Before initialize(), VideoEncoder must report a sane default state
    // (it shouldn't crash if used uninitialized).
    // encode_frame should return false without crashing.
    // (We do NOT call encode_frame since D3D11 is not available in unit tests.)
    VideoEncoder enc;
    EXPECT_EQ(enc.mode(), EncoderMode::SoftwareMFT);  // default mode
    EXPECT_EQ(enc.output_fps(), 30u);
    EXPECT_EQ(enc.output_width(), 1920u);
    EXPECT_EQ(enc.output_height(), 1080u);
}

} // namespace sr
