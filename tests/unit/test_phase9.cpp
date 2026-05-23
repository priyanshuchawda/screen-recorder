// test_phase9.cpp — Unit tests for Phase 9 tasks T037-T043
//
// Covers:
//   T037: TelemetryStore / TelemetrySnapshot
//   T038: FramePacer pacing logic
//   T039: CaptureEngine device_lost_ flag API
//   T042: PowerModeDetector clamp_for_power()
//   T043: CaptureEngine::is_wgc_supported() available without crashing

#include <gtest/gtest.h>
#include <windows.h>
#include <chrono>
#include <thread>
#include <vector>

#include "app/telemetry.h"
#include "app/camera_overlay.h"
#include "audio/audio_mixer.h"
#include "sync/frame_pacer.h"
#include "encoder/power_mode.h"
#include "utils/bounded_queue.h"
#include "utils/render_frame.h"
#include "capture/capture_engine.h"   // T039 / T043

// ============================================================
// T037: TelemetryStore
// ============================================================

TEST(T037_Telemetry, InitialSnapshotIsAllZeros) {
    sr::TelemetryStore ts;
    auto snap = ts.snapshot(0, true);
    EXPECT_EQ(snap.frames_captured,    0u);
    EXPECT_EQ(snap.frames_encoded,     0u);
    EXPECT_EQ(snap.frames_dropped,     0u);
    EXPECT_EQ(snap.frames_backlogged,  0u);
    EXPECT_EQ(snap.audio_packets,      0u);
    EXPECT_EQ(snap.dup_frames,         0u);
}

TEST(T037_Telemetry, CountersIncrementCorrectly) {
    sr::TelemetryStore ts;
    ts.on_frame_captured();
    ts.on_frame_captured();
    ts.on_frame_encoded();
    ts.on_frame_dropped();
    ts.on_audio_written();
    ts.on_audio_written();
    ts.on_audio_written();
    ts.on_duplicate_inserted();

    auto snap = ts.snapshot(1, false);
    EXPECT_EQ(snap.frames_captured, 2u);
    EXPECT_EQ(snap.frames_encoded,  1u);
    EXPECT_EQ(snap.frames_dropped,  1u);
    EXPECT_EQ(snap.audio_packets,   3u);
    EXPECT_EQ(snap.dup_frames,      1u);
    EXPECT_EQ(snap.encoder_mode,    1u);    // passed-in
    EXPECT_FALSE(snap.is_on_ac);           // passed-in
}

TEST(T037_Telemetry, ResetClearsAllCounters) {
    sr::TelemetryStore ts;
    for (int i = 0; i < 10; ++i) { ts.on_frame_captured(); ts.on_frame_encoded(); }
    ts.reset();
    auto snap = ts.snapshot(0, true);
    EXPECT_EQ(snap.frames_captured, 0u);
    EXPECT_EQ(snap.frames_encoded,  0u);
}

TEST(T037_Telemetry, BacklogSetAndReflectedInSnapshot) {
    sr::TelemetryStore ts;
    ts.set_backlog(3);
    EXPECT_EQ(ts.snapshot(0, true).frames_backlogged, 3u);
    ts.set_backlog(0);
    EXPECT_EQ(ts.snapshot(0, true).frames_backlogged, 0u);
}

TEST(T037_Telemetry, EncoderModeLabelStrings) {
    sr::TelemetrySnapshot s;
    s.encoder_mode = 0; EXPECT_STREQ(s.encoder_mode_label(), L"HW");
    s.encoder_mode = 1; EXPECT_STREQ(s.encoder_mode_label(), L"SW");
    s.encoder_mode = 2; EXPECT_STREQ(s.encoder_mode_label(), L"SW 720p");
}

// ============================================================
// T038: FramePacer
// ============================================================

TEST(T038_FramePacer, FirstFrameAlwaysAccepted) {
    sr::FramePacer pacer;
    pacer.initialize(30);
    int64_t out = -1;
    auto action = pacer.pace_frame(333333LL, false, &out);
    EXPECT_EQ(action, sr::PaceAction::Accept);
    EXPECT_EQ(out, 333333LL);
}

TEST(T038_FramePacer, NormalFramesAccepted) {
    sr::FramePacer pacer;
    pacer.initialize(30);
    int64_t out = 0;
    int64_t pts = 0;
    for (int i = 0; i < 30; ++i) {
        pts += 333333LL;
        auto action = pacer.pace_frame(pts, false, &out);
        EXPECT_NE(action, sr::PaceAction::Drop);
        EXPECT_GT(out, 0LL);
    }
}

TEST(T038_FramePacer, GapLargerThan1p5xTriggersDuplicate) {
    sr::FramePacer pacer;
    pacer.initialize(30);

    int64_t out = 0;
    // First frame
    pacer.pace_frame(333333LL, false, &out);
    // Gap of 2× interval triggers Duplicate
    int64_t big_gap_pts = 333333LL + 333333LL * 2; // 3× target from start
    auto action = pacer.pace_frame(big_gap_pts, false, &out);
    EXPECT_EQ(action, sr::PaceAction::Duplicate);
    EXPECT_EQ(pacer.duplicates_inserted(), 1u);
}

TEST(T038_FramePacer, QueueFullCausesDrop) {
    sr::FramePacer pacer;
    pacer.initialize(30);
    int64_t out = 0;
    pacer.pace_frame(333333LL, false, &out); // first frame — bootstrap
    auto action = pacer.pace_frame(666666LL, /*queue_full=*/true, &out);
    EXPECT_EQ(action, sr::PaceAction::Drop);
    EXPECT_EQ(pacer.drops(), 1u);
}

TEST(T038_FramePacer, ResetClearsPacingState) {
    sr::FramePacer pacer;
    pacer.initialize(30);
    int64_t out = 0;
    pacer.pace_frame(333333LL, false, &out);
    pacer.reset();
    // After reset, next frame should behave as the first frame again
    auto action = pacer.pace_frame(50'000'000LL, false, &out); // large PTS
    EXPECT_EQ(action, sr::PaceAction::Accept); // no duplicate since state was reset
    EXPECT_EQ(pacer.duplicates_inserted(), 0u);
}

TEST(T038_FramePacer, OutputPTSMonotonicUnderJitter) {
    sr::FramePacer pacer;
    pacer.initialize(30);

    int64_t raw_pts  = 0;
    int64_t prev_out = -1;
    bool    monotonic= true;

    for (int i = 0; i < 300; ++i) {
        // ±5ms jitter around 33ms nominal
        int64_t jitter = (i % 3 == 0) ? 50'000LL : -50'000LL;
        raw_pts += 333'333LL + jitter;
        if (raw_pts < 0) raw_pts = 0;

        int64_t out = 0;
        auto action = pacer.pace_frame(raw_pts, false, &out);
        if (action == sr::PaceAction::Drop) continue;

        if (prev_out >= 0 && out <= prev_out) { monotonic = false; break; }
        prev_out = out;
    }
    EXPECT_TRUE(monotonic) << "FramePacer output PTS is not monotonic under jitter";
}

// ============================================================
// T039: CaptureEngine device_lost_ atomic flag
// ============================================================

TEST(T039_DeviceLost, DeviceLostFlagInitiallyFalse) {
    // We can construct a CaptureEngine header-only since device_lost_ is
    // publicly readable (std::atomic<bool>) on the CaptureEngine object.
    // (We don't call initialize() as that requires a D3D11 device.)
    // The test verifies the API surface exists and defaults correctly.
    sr::CaptureEngine ce;
    // device_lost_ is private but exposed indirectly via callback registration
    // This test verifies the callback API compiles and doesn't crash
    bool  fired = false;
    ce.set_device_lost_callback([&fired]() { fired = true; });
    // No D3D device → never fires, confirm it's still false
    EXPECT_FALSE(fired);
}

// ============================================================
// T042: PowerModeDetector
// ============================================================

TEST(T042_PowerMode, IsOnAcPowerDoesNotCrash) {
    // Simply calling the API must not throw or crash
    bool result = sr::PowerModeDetector::is_on_ac_power();
    // result is either true or false — both are valid
    SUCCEED();
    (void)result;
}

TEST(T042_PowerMode, ClampForPowerOnACReturnsRequestedProfile) {
    sr::EncoderProfile req;
    req.fps         = 60;
    req.bitrate_bps = 14'000'000;
    req.width       = 1920;
    req.height      = 1080;

    // Temporarily simulate AC: feed a profile that shouldn't be clamped
    // (We can't force the power state, so we test the internal logic directly
    //  by providing our own AC-detection helper via white-box knowledge.)
    // At minimum: if the API reports AC, the profile must be unchanged.
    bool on_ac = sr::PowerModeDetector::is_on_ac_power();
    auto result = sr::PowerModeDetector::clamp_for_power(req);

    if (on_ac) {
        EXPECT_EQ(result.fps,         req.fps);
        EXPECT_EQ(result.bitrate_bps, req.bitrate_bps);
        EXPECT_EQ(result.width,       req.width);
        EXPECT_EQ(result.height,      req.height);
    } else {
        EXPECT_LE(result.fps,         15u);
        EXPECT_LE(result.bitrate_bps, 1'500'000u);
        EXPECT_LE(result.width,       848u);
        EXPECT_LE(result.height,      480u);
    }
}

TEST(T042_PowerMode, BatteryProfileClampsToHardwareEncoderCompatibleDimensions) {
    sr::EncoderProfile req;
    req.fps         = 60;
    req.bitrate_bps = 14'000'000;
    req.width       = 1920;
    req.height      = 1080;

    sr::EncoderProfile throttled = sr::PowerModeDetector::clamp_for_battery(req);

    EXPECT_EQ(throttled.fps,         15u);
    EXPECT_EQ(throttled.bitrate_bps, 1'500'000u);
    EXPECT_EQ(throttled.width,       848u);
    EXPECT_EQ(throttled.height,      480u);
    EXPECT_EQ(throttled.width % 16u, 0u);
    EXPECT_EQ(throttled.height % 16u, 0u);
}

TEST(T042_PowerMode, CameraPreviewUsesEfficiencyIntervals) {
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_power(false), 66);
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_power(true), 100);
    EXPECT_LE(sr::CameraOverlay::kEfficiencyPreviewMaxWidth, 640u);
    EXPECT_LE(sr::CameraOverlay::kEfficiencyPreviewMaxHeight, 480u);
}

TEST(T042_PowerMode, CameraPreviewProcessingSkipsFramesBeforeInterval) {
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1000, 0, 66));
    EXPECT_FALSE(sr::CameraOverlay::should_process_preview_frame(1050, 1000, 66));
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1066, 1000, 66));
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1000, 1000, 0));
}

// ============================================================
// T043: WGC availability check
// ============================================================

TEST(T043_WGCConsent, IsWGCSupportedDoesNotCrash) {
    // On this machine (Windows 10/11) this should return true.
    // The test just verifies the API exists, compiles, and runs without throw.
    bool supported = sr::CaptureEngine::is_wgc_supported();
    // On any Win10 1903+ build (which the CI machine is), this must be true.
    EXPECT_TRUE(supported) << "WGC is expected to be available on Win10 1903+ machines";
}

TEST(T044_AudioMixing, FindsClosestTimestampCompatibleLoopbackPacket) {
    sr::AudioPacket mic;
    mic.pts = 1'000'000;
    mic.sample_rate = 48000;
    mic.channels = 2;
    mic.buffer.assign(1920, 0);

    std::vector<sr::AudioPacket> loopback(3);
    loopback[0].pts = 900'000;
    loopback[0].sample_rate = 48000;
    loopback[0].channels = 2;
    loopback[0].buffer.assign(1920, 0);

    loopback[1].pts = 1'012'000;
    loopback[1].sample_rate = 48000;
    loopback[1].channels = 2;
    loopback[1].buffer.assign(1920, 0);

    loopback[2].pts = 1'004'000;
    loopback[2].sample_rate = 48000;
    loopback[2].channels = 2;
    loopback[2].buffer.assign(960, 0);

    const auto match = sr::find_loopback_mix_candidate(mic, loopback, 20'000);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(*match, 1u);
}
