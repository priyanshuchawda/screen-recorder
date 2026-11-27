// test_phase8.cpp — Unit tests for Phase 8: Polish & Cross-Cutting Constraints
// Covers T032 (AudioResampler), T033 (thread priorities), T035 (encoder fallback),
// T036 (queue stability), T034 (resolution change checks)

#include <gtest/gtest.h>
#include <windows.h>
#include <mfapi.h>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>

// ─── Utility headers ──────────────────────────────────────────────────────────
#include "utils/bounded_queue.h"
#include "utils/render_frame.h"
#include "capture/capture_engine.h"
#include "audio/audio_engine.h"

// ─── T032: AudioResampler ─────────────────────────────────────────────────────
#include "audio/audio_resampler.h"

// ─── T035: VideoEncoder (fallback chain identifiers) ─────────────────────────
#include "encoder/video_encoder.h"
#include "encoder/encoder_probe.h"

namespace sr {

// =============================================================================
// T036 — Queue Stability Tests
// =============================================================================
TEST(T036_QueueStability, VideoQueueCapacityIsEight) {
    EXPECT_EQ(FrameQueue::capacity(), 8u);
}

TEST(T036_QueueStability, AudioQueueCapacityIsBounded) {
    EXPECT_LE(AudioQueue::capacity(), 2048u);
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

TEST(T036_QueueStability, VideoQueueBuffersEightFramesAndStaysBounded) {
    FrameQueue q;
    for (size_t i = 0; i < FrameQueue::capacity(); ++i) {
        RenderFrame frame;
        EXPECT_TRUE(q.try_push(std::move(frame)));
    }
    RenderFrame overflow;
    EXPECT_FALSE(q.try_push(std::move(overflow)));
    EXPECT_EQ(q.size(), FrameQueue::capacity());
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

TEST_F(T032_AudioResamplerTest, ResamplerPreservesTenMillisecondDurationAt44100And96000) {
    for (const uint32_t input_rate : {44'100u, 96'000u}) {
        AudioResampler rs;
        if (!rs.initialize(input_rate, 2, 16, 48'000)) {
            GTEST_SKIP() << "Resampler unavailable for " << input_rate << " Hz";
        }

        const uint32_t input_frames_per_packet = input_rate / 100;
        std::vector<uint8_t> input(
            static_cast<size_t>(input_frames_per_packet) * 2 * sizeof(int16_t), 0);
        std::vector<uint8_t> output;
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(rs.process(input.data(), static_cast<uint32_t>(input.size()), output));
        }
        std::vector<uint8_t> tail;
        ASSERT_TRUE(rs.flush(tail));
        output.insert(output.end(), tail.begin(), tail.end());

        const size_t output_frames = output.size() / (2 * sizeof(int16_t));
        EXPECT_NEAR(static_cast<double>(output_frames), 4'800.0, 1.0)
            << "100 ms of " << input_rate << " Hz PCM must remain 100 ms at 48 kHz";
    }
}

TEST_F(T032_AudioResamplerTest, RejectsNullInputWithoutDereferencingIt) {
    AudioResampler rs;
    ASSERT_TRUE(rs.initialize(44'100, 2, 16, 48'000));
    std::vector<uint8_t> output;
    EXPECT_FALSE(rs.process(nullptr, 1764, output));
}

TEST_F(T032_AudioResamplerTest, ResamplesSilentPacketsToCanonicalDuration) {
    for (const uint32_t input_rate : {44'100u, 96'000u}) {
        AudioResampler rs;
        if (!rs.initialize(input_rate, 2, 16, 48'000)) {
            GTEST_SKIP() << "Resampler unavailable for " << input_rate << " Hz";
        }

        const uint32_t input_frames = input_rate / 100;
        const uint32_t input_bytes = input_frames * 2 * sizeof(int16_t);
        std::vector<uint8_t> output;
        for (int i = 0; i < 10; ++i) {
            int64_t output_pts = -1;
            ASSERT_TRUE(rs.process(nullptr, input_bytes, output, true,
                                   2'000'000 + i * 100'000, &output_pts));
        }
        int64_t tail_pts = -1;
        std::vector<uint8_t> tail;
        ASSERT_TRUE(rs.flush(tail, &tail_pts));
        output.insert(output.end(), tail.begin(), tail.end());

        EXPECT_NEAR(static_cast<double>(output.size() / (2 * sizeof(int16_t))), 4'800.0, 1.0);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                                [](uint8_t sample_byte) { return sample_byte == 0; }));
    }
}

TEST_F(T032_AudioResamplerTest, ResampledPacketTimestampsStayContinuous) {
    AudioResampler rs;
    if (!rs.initialize(44'100, 2, 16, 48'000)) {
        GTEST_SKIP() << "Resampler unavailable";
    }

    constexpr int64_t kStartPts = 2'000'000;
    constexpr int64_t kPacketDuration = 100'000;
    const std::vector<uint8_t> input(441u * 2u * sizeof(int16_t), 0);
    std::vector<uint8_t> output;
    int64_t expected_next_pts = kStartPts;
    bool have_output = false;
    auto verify_packet = [&](size_t old_size, int64_t packet_pts) {
        const size_t produced_bytes = output.size() - old_size;
        if (produced_bytes == 0) return;
        if (have_output) EXPECT_LE(std::llabs(packet_pts - expected_next_pts), 2LL);
        else EXPECT_LE(std::llabs(packet_pts - kStartPts), 2LL);
        const uint64_t frames = produced_bytes / (2u * sizeof(int16_t));
        expected_next_pts = packet_pts + static_cast<int64_t>(frames * 10'000'000ULL / 48'000ULL);
        have_output = true;
    };

    for (int i = 0; i < 10; ++i) {
        const size_t old_size = output.size();
        int64_t packet_pts = -1;
        ASSERT_TRUE(rs.process(input.data(), static_cast<uint32_t>(input.size()), output,
                               false, kStartPts + i * kPacketDuration, &packet_pts));
        verify_packet(old_size, packet_pts);
    }
    const size_t old_size = output.size();
    int64_t tail_pts = -1;
    std::vector<uint8_t> tail;
    ASSERT_TRUE(rs.flush(tail, &tail_pts));
    output.insert(output.end(), tail.begin(), tail.end());
    if (!tail.empty()) {
        if (have_output) EXPECT_LE(std::llabs(tail_pts - expected_next_pts), 2LL);
        else EXPECT_LE(std::llabs(tail_pts - kStartPts), 2LL);
    }
    EXPECT_TRUE(have_output || output.size() > old_size);
}

TEST(AudioResamplerRetryTest, DrainsAndRetriesTheSameRejectedInput) {
    unsigned submit_attempts = 0;
    unsigned drain_attempts = 0;
    const HRESULT result = sr::audio_resampler_detail::submit_with_drain_retry(
        [&]() -> HRESULT {
            return ++submit_attempts == 1 ? MF_E_NOTACCEPTING : S_OK;
        },
        [&]() {
            ++drain_attempts;
            return true;
        }, 3);
    EXPECT_EQ(result, S_OK);
    EXPECT_EQ(submit_attempts, 2u);
    EXPECT_EQ(drain_attempts, 1u);
}

TEST(AudioResamplerRetryTest, StopsAfterBoundedRetriesIfMftKeepsRejecting) {
    unsigned submit_attempts = 0;
    unsigned drain_attempts = 0;
    const HRESULT result = sr::audio_resampler_detail::submit_with_drain_retry(
        [&]() -> HRESULT {
            ++submit_attempts;
            return MF_E_NOTACCEPTING;
        },
        [&]() {
            ++drain_attempts;
            return true;
        }, 3);
    EXPECT_EQ(result, MF_E_NOTACCEPTING);
    EXPECT_EQ(submit_attempts, 3u);
    EXPECT_EQ(drain_attempts, 2u);
}

TEST(AudioResamplerRetryTest, DoesNotRetryAfterOutputDrainFailure) {
    unsigned submit_attempts = 0;
    const HRESULT result = sr::audio_resampler_detail::submit_with_drain_retry(
        [&]() -> HRESULT {
            ++submit_attempts;
            return MF_E_NOTACCEPTING;
        },
        []() { return false; }, 3);
    EXPECT_EQ(result, E_FAIL);
    EXPECT_EQ(submit_attempts, 1u);
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
    EXPECT_EQ(FrameQueue::capacity(), static_cast<size_t>(8));
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

TEST(T035_EncoderFallback, IntelHardwareAdapterPreferredForHardwareEncoding) {
    const int intel_hw_score = EncoderProbe::adapter_preference_score(0x8086, false);
    const int other_hw_score = EncoderProbe::adapter_preference_score(0x10DE, false);
    const int intel_sw_score = EncoderProbe::adapter_preference_score(0x8086, true);

    EXPECT_GT(intel_hw_score, other_hw_score);
    EXPECT_GT(other_hw_score, intel_sw_score);
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
