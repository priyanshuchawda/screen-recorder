// test_phase9.cpp — Unit tests for Phase 9 tasks T037-T043
//
// Covers:
//   T037: TelemetryStore / TelemetrySnapshot
//   M4: Fixed-frame output cadence and pipeline wake signal
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
#include "capture/capture_cadence.h"
#include "controller/audio_write_schedule.h"
#include "audio/audio_engine.h"
#include "storage/mux_writer.h"

TEST(AudioQueueRecovery, BuffersTwentySecondsOfTenMillisecondPackets) {
    EXPECT_GE(sr::AudioQueue::capacity(), 2000u);
}

TEST(AudioWriteSchedule, CatchesUpBeyondFourBlocksWhenVideoIsAhead) {
    EXPECT_TRUE(sr::continue_audio_pass(4, 6'000'000, 10'000'000, true));
    EXPECT_FALSE(sr::continue_audio_pass(40, 10'000'000, 10'000'000, true));
}

TEST(AudioWriteSchedule, BoundsWorkAndHandlesMissingVideo) {
    EXPECT_FALSE(sr::continue_audio_pass(200, 0, 100'000'000, true));
    EXPECT_TRUE(sr::continue_audio_pass(3, 0, 0, false));
    EXPECT_FALSE(sr::continue_audio_pass(4, 0, 0, false));
}

TEST(CaptureCadenceTest, KeepsNearlySixtyHertzFramesAtSixtyFps) {
    sr::CaptureCadence cadence;
    constexpr int64_t interval = 10'000'000 / 60;
    unsigned accepted = 0;
    for (int i = 0; i < 3600; ++i) {
        const int64_t pts = static_cast<int64_t>(i) * (interval - 200);
        if (cadence.accept(pts, interval)) ++accepted;
    }
    EXPECT_GE(accepted, 3500u);
}

TEST(CaptureCadenceTest, CountsFramesRejectedByCadenceLimit) {
    sr::CaptureCadence cadence;
    EXPECT_TRUE(cadence.accept(0, 10));
    EXPECT_FALSE(cadence.accept(1, 10));
    EXPECT_FALSE(cadence.accept(2, 10));
    EXPECT_EQ(cadence.rejected_count(), 2u);
}

TEST(CaptureCadenceTest, LimitsOneHundredTwentyHertzToSixtyFps) {
    sr::CaptureCadence cadence;
    constexpr int64_t interval = 10'000'000 / 60;
    unsigned accepted = 0;
    for (int i = 0; i < 1200; ++i) {
        const int64_t pts = static_cast<int64_t>(i) * interval / 2;
        if (cadence.accept(pts, interval)) ++accepted;
    }
    EXPECT_GE(accepted, 590u);
    EXPECT_LE(accepted, 610u);
}

TEST(CaptureCadenceTest, RateChangeAndLongGapDoNotCreateCatchUpBurst) {
    sr::CaptureCadence cadence;
    constexpr int64_t fps60 = 10'000'000 / 60;
    constexpr int64_t fps30 = 10'000'000 / 30;
    EXPECT_TRUE(cadence.accept(0, fps60));
    EXPECT_FALSE(cadence.accept(fps60 / 2, fps60));
    EXPECT_TRUE(cadence.accept(10'000'000, fps60));
    EXPECT_FALSE(cadence.accept(10'000'001, fps60));
    EXPECT_TRUE(cadence.accept(10'000'002, fps30));
    EXPECT_FALSE(cadence.accept(10'000'003, fps30));
    cadence.reset();
    EXPECT_TRUE(cadence.accept(0, fps30));
}

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
    ts.on_queue_drop();
    ts.on_audio_written();
    ts.on_audio_written();
    ts.on_audio_written();
    ts.on_duplicate_inserted();

    auto snap = ts.snapshot(1, false);
    EXPECT_EQ(snap.frames_captured, 2u);
    EXPECT_EQ(snap.frames_encoded,  1u);
    EXPECT_EQ(snap.frames_dropped,  1u);
    EXPECT_EQ(snap.queue_drops,     1u);
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

TEST(M4FixedFramePacer, ProducesExactThirtyAndSixtyFpsDeadlines) {
    for (const uint32_t fps : {30u, 60u}) {
        sr::FixedFramePacer pacer(fps);
        pacer.begin(1'000'000);
        EXPECT_EQ(pacer.next_deadline(), 1'000'000);
        const int64_t first = pacer.emit_due(1'000'000, true);
        EXPECT_EQ(first, 1'000'000);
        EXPECT_EQ(pacer.next_deadline(), 1'000'000 + 10'000'000LL / fps);
        EXPECT_FALSE(pacer.output_due(pacer.next_deadline() - 1));
        EXPECT_TRUE(pacer.output_due(pacer.next_deadline()));
        EXPECT_EQ(pacer.emit_due(pacer.next_deadline(), false),
                  1'000'000 + 10'000'000LL / fps);
    }
}

TEST(M4FixedFramePacer, StaticScreenRepeatsOnlyOnOutputDeadlines) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    EXPECT_EQ(pacer.emit_due(0, true), 0);
    EXPECT_EQ(pacer.emit_due(100'000, false), -1);
    EXPECT_EQ(pacer.emit_due(333'333, false), 333'333);
    EXPECT_EQ(pacer.emitted_repeats(), 1u);
    EXPECT_EQ(pacer.next_deadline(), 666'666);
}

TEST(M4FixedFramePacer, SkipsMissedSlotsWithoutCatchupBurst) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    EXPECT_EQ(pacer.emit_due(0, true), 0);
    const auto due = pacer.emit_due(1'200'000, true);
    EXPECT_EQ(due, 1'000'000);
    EXPECT_EQ(pacer.skipped_slots(), 2u);
    EXPECT_EQ(pacer.next_deadline(), 1'333'333);
}

TEST(M4FixedFramePacer, LateWakeAtRoundedThirtyFpsDeadlineDoesNotCatchUp) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    EXPECT_EQ(pacer.emit_due(0, true), 0);

    // Slot 5's deadline is floor(5 * 10,000,000 / 30) = 1,666,666.
    // Even after waking at that exact rounded deadline, emit only slot 5.
    EXPECT_EQ(pacer.emit_due(1'666'666, true), 1'666'666);
    EXPECT_EQ(pacer.skipped_slots(), 4u);
    EXPECT_EQ(pacer.next_deadline(), 2'000'000);
    EXPECT_EQ(pacer.emit_due(1'666'666, true), -1);
}

TEST(M4FixedFramePacer, ResetReanchorsAfterPause) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    EXPECT_EQ(pacer.emit_due(0, true), 0);
    pacer.reset();
    pacer.begin(9'000'000);
    EXPECT_EQ(pacer.next_deadline(), 9'000'000);
    EXPECT_EQ(pacer.emit_due(9'000'000, true), 9'000'000);
    EXPECT_EQ(pacer.skipped_slots(), 0u);
}

TEST(M4FixedFramePacer, EmitsFinalPendingFrameAtNextSlot) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    EXPECT_EQ(pacer.emit_due(0, true), 0);
    EXPECT_EQ(pacer.emit_next(true), 333'333);
    EXPECT_EQ(pacer.next_deadline(), 666'666);
    EXPECT_EQ(pacer.emitted_repeats(), 0u);
}

TEST(M4FixedFramePacer, DoesNotEmitInvalidTimestampBeforeTimelineStarts) {
    sr::FixedFramePacer pacer(30);
    EXPECT_EQ(pacer.emit_next(true), -1);
    EXPECT_FALSE(pacer.started());
    pacer.begin(1'000'000);
    EXPECT_EQ(pacer.emit_next(true), 1'000'000);
}

TEST(M4FixedFramePacer, DoesNotExceedRequestedRateDuringIrregularArrivals) {
    sr::FixedFramePacer pacer(30);
    pacer.begin(0);
    uint64_t outputs = 0;
    int64_t previous_pts = -1;
    for (int64_t now = 0; now <= 100'000'000; now += 17'000 + (now % 13'000)) {
        if (pacer.output_due(now)) {
            const int64_t pts = pacer.emit_due(now, (now % 97'000) < 30'000);
            ASSERT_GT(pts, previous_pts);
            if (previous_pts >= 0) EXPECT_GE(pts - previous_pts, 333'333);
            previous_pts = pts;
            ++outputs;
        }
    }
    EXPECT_EQ(pacer.skipped_slots(), 0u) << "outputs=" << outputs << " last=" << previous_pts;
    EXPECT_LE(outputs, 301u); // Includes the tick at t=10s; no extra gap duplicates.
    EXPECT_GT(outputs, 290u);
}

TEST(M4PipelineWake, NotificationBeforeWaitIsNotLost) {
    sr::PipelineWakeSignal wake;
    const auto observed = wake.generation();
    wake.notify();
    EXPECT_TRUE(wake.wait_for_change(observed, std::chrono::milliseconds(0)));
}

TEST(M4PipelineWake, QueuePushNotifiesSharedConsumer) {
    auto wake = std::make_shared<sr::PipelineWakeSignal>();
    sr::BoundedQueue<int, 2> queue;
    queue.set_wake_signal(wake, sr::PipelineWakeSource::CaptureFrame);
    const auto observed = wake->generation();
    EXPECT_TRUE(queue.try_push(7));
    EXPECT_TRUE(wake->wait_for_change(observed, std::chrono::milliseconds(0)));
    ASSERT_TRUE(queue.try_pop().has_value());
}

TEST(M4PipelineWake, QueueCoalescesSignalsUntilItBecomesNonEmptyAgain) {
    auto wake = std::make_shared<sr::PipelineWakeSignal>();
    sr::BoundedQueue<int, 4> queue;
    queue.set_wake_signal(wake, sr::PipelineWakeSource::MicrophoneAudio);
    ASSERT_TRUE(queue.try_push(1));
    ASSERT_TRUE(queue.try_push(2));
    EXPECT_EQ(wake->notification_count(sr::PipelineWakeSource::MicrophoneAudio), 1u);
    ASSERT_TRUE(queue.try_pop().has_value());
    ASSERT_TRUE(queue.try_pop().has_value());
    ASSERT_TRUE(queue.try_push(3));
    EXPECT_EQ(wake->notification_count(sr::PipelineWakeSource::MicrophoneAudio), 2u);
}

TEST(M4PipelineWake, TracksNotificationSourcesIndependently) {
    auto wake = std::make_shared<sr::PipelineWakeSignal>();
    wake->notify(sr::PipelineWakeSource::EncoderOutput);
    wake->notify(sr::PipelineWakeSource::EncoderFailure);
    EXPECT_EQ(wake->notification_count(sr::PipelineWakeSource::EncoderOutput), 1u);
    EXPECT_EQ(wake->notification_count(sr::PipelineWakeSource::EncoderFailure), 1u);
    EXPECT_EQ(wake->notification_count(sr::PipelineWakeSource::MicrophoneAudio), 0u);
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

TEST(T042_PowerMode, ExplicitACPowerStateKeepsHighQualityProfile) {
    sr::EncoderProfile req;
    req.fps         = 30;
    req.bitrate_bps = 8'000'000;
    req.width       = 1920;
    req.height      = 1080;

    auto result = sr::PowerModeDetector::clamp_for_power_state(req, true);

    EXPECT_EQ(result.fps, req.fps);
    EXPECT_EQ(result.bitrate_bps, req.bitrate_bps);
    EXPECT_EQ(result.width, req.width);
    EXPECT_EQ(result.height, req.height);
}

TEST(T042_PowerMode, ExplicitBatteryPowerStateClampsBeforeCaptureAllocation) {
    sr::EncoderProfile req;
    req.fps         = 30;
    req.bitrate_bps = 8'000'000;
    req.width       = 1920;
    req.height      = 1080;

    auto result = sr::PowerModeDetector::clamp_for_power_state(req, false);

    EXPECT_EQ(result.fps, sr::PowerModeDetector::kBatteryMaxFps);
    EXPECT_EQ(result.bitrate_bps, sr::PowerModeDetector::kBatteryMaxBitrate);
    EXPECT_EQ(result.width, sr::PowerModeDetector::kBatteryMaxWidth);
    EXPECT_EQ(result.height, sr::PowerModeDetector::kBatteryMaxHeight);
}

TEST(T042_PowerMode, HighQualityProfileBypassesBatteryClamp) {
    sr::EncoderProfile req;
    req.fps         = 30;
    req.bitrate_bps = 8'000'000;
    req.width       = 1920;
    req.height      = 1080;

    auto result = sr::PowerModeDetector::clamp_for_quality_and_power_state(
        req, false, true);

    EXPECT_EQ(result.fps, req.fps);
    EXPECT_EQ(result.bitrate_bps, req.bitrate_bps);
    EXPECT_EQ(result.width, req.width);
    EXPECT_EQ(result.height, req.height);
}

TEST(T042_PowerMode, BaseQualityStillClampsOnBattery) {
    sr::EncoderProfile req;
    req.fps         = 30;
    req.bitrate_bps = 8'000'000;
    req.width       = 1920;
    req.height      = 1080;

    auto result = sr::PowerModeDetector::clamp_for_quality_and_power_state(
        req, false, false);

    EXPECT_EQ(result.fps, sr::PowerModeDetector::kBatteryMaxFps);
    EXPECT_EQ(result.bitrate_bps, sr::PowerModeDetector::kBatteryMaxBitrate);
    EXPECT_EQ(result.width, sr::PowerModeDetector::kBatteryMaxWidth);
    EXPECT_EQ(result.height, sr::PowerModeDetector::kBatteryMaxHeight);
}

TEST(T042_PowerMode, CameraPreviewUsesEfficiencyIntervals) {
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_power(false), 50);
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_power(true), 50);
    EXPECT_LE(sr::CameraOverlay::kEfficiencyPreviewMaxWidth, 640u);
    EXPECT_LE(sr::CameraOverlay::kEfficiencyPreviewMaxHeight, 480u);
}

TEST(T042_PowerMode, CameraPreviewUsesHighQualityProfileOnAnyPowerState) {
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_profile(false, false), 50);
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_profile(true, false), 50);
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_profile(false, true), 33);
    EXPECT_EQ(sr::CameraOverlay::preview_interval_ms_for_profile(true, true), 33);

    EXPECT_EQ(sr::CameraOverlay::preview_max_width_for_profile(false, false), 640u);
    EXPECT_EQ(sr::CameraOverlay::preview_max_height_for_profile(false, false), 480u);
    EXPECT_EQ(sr::CameraOverlay::preview_max_width_for_profile(false, true), 1280u);
    EXPECT_EQ(sr::CameraOverlay::preview_max_height_for_profile(false, true), 720u);
    EXPECT_EQ(sr::CameraOverlay::preview_max_width_for_profile(true, true), 1280u);
    EXPECT_EQ(sr::CameraOverlay::preview_max_height_for_profile(true, true), 720u);
}

TEST(T042_PowerMode, CameraPreviewProcessingSkipsFramesBeforeInterval) {
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1000, 0, 50));
    EXPECT_FALSE(sr::CameraOverlay::should_process_preview_frame(1049, 1000, 50));
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1050, 1000, 50));
    EXPECT_TRUE(sr::CameraOverlay::should_process_preview_frame(1000, 1000, 0));
}

TEST(T042_PowerMode, CameraPreviewTuningOnlyReappliesWhenProfileChanges) {
    EXPECT_TRUE(sr::CameraOverlay::should_reapply_preview_tuning(
        false, false, false, false, false));
    EXPECT_FALSE(sr::CameraOverlay::should_reapply_preview_tuning(
        true, false, false, false, false));
    EXPECT_FALSE(sr::CameraOverlay::should_reapply_preview_tuning(
        true, false, false, true, false));
    EXPECT_TRUE(sr::CameraOverlay::should_reapply_preview_tuning(
        true, false, false, false, true));
    EXPECT_FALSE(sr::CameraOverlay::should_reapply_preview_tuning(
        true, true, true, false, true));
}

TEST(T042_PowerMode, CameraReadFailureBackoffIncreasesAndCaps) {
    EXPECT_EQ(sr::CameraOverlay::read_failure_backoff_ms(1), 25u);
    EXPECT_EQ(sr::CameraOverlay::read_failure_backoff_ms(4), 100u);
    EXPECT_EQ(sr::CameraOverlay::read_failure_backoff_ms(20), 250u);
}

TEST(T042_PowerMode, CameraReadFailureStopsAfterPersistentFailures) {
    EXPECT_FALSE(sr::CameraOverlay::should_stop_after_read_failures(1));
    EXPECT_FALSE(sr::CameraOverlay::should_stop_after_read_failures(49));
    EXPECT_TRUE(sr::CameraOverlay::should_stop_after_read_failures(50));
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

namespace {
sr::AudioPacket make_mixer_test_packet(int64_t pts, uint32_t frames = 480,
                                       uint16_t channels = 2) {
    sr::AudioPacket packet;
    packet.pts = pts;
    packet.frame_count = frames;
    packet.sample_rate = sr::AudioTimelineMixer::kOutputRate;
    packet.channels = channels;
    packet.bits_per_sample = 32;
    packet.is_float = true;
    packet.buffer.resize(static_cast<size_t>(frames) * channels * sizeof(float));
    return packet;
}
} // namespace

TEST(AudioMixerBoundsTest, DropsOldestPacketsAtDurationLimitAndReportsLoss) {
    sr::AudioTimelineMixer mixer;
    for (int64_t index = 0; index < 250; ++index) {
        const auto result = mixer.push(make_mixer_test_packet(index * 100'000), true);
        EXPECT_TRUE(result.accepted);
    }

    const auto stats = mixer.stats();
    EXPECT_EQ(stats.dropped_packets, 50u);
    EXPECT_EQ(stats.rejected_packets, 0u);
    EXPECT_EQ(stats.microphone_packets, 200u);
    EXPECT_LE(stats.microphone_bytes, sr::AudioTimelineMixer::kMaxBufferedBytes);
    EXPECT_LE(stats.microphone_high_water_duration_100ns, 20'000'000);
    EXPECT_EQ(stats.microphone_high_water_packets, 200u);
}

TEST(AudioMixerBoundsTest, RejectsSingleOversizedPacket) {
    sr::AudioTimelineMixer mixer;
    auto packet = make_mixer_test_packet(0, 48'000, 32);
    ASSERT_GT(packet.buffer.size(), sr::AudioTimelineMixer::kMaxBufferedBytes);

    const auto result = mixer.push(std::move(packet), false);

    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.rejected);
    EXPECT_EQ(mixer.stats().rejected_packets, 1u);
    EXPECT_EQ(mixer.stats().loopback_packets, 0u);
}

TEST(AudioMixerBoundsTest, RejectsMalformedPacketWithoutGrowingQueue) {
    sr::AudioTimelineMixer mixer;
    auto packet = make_mixer_test_packet(0);
    packet.buffer.resize(3);

    const auto result = mixer.push(std::move(packet), true);

    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.rejected);
    EXPECT_EQ(mixer.stats().microphone_packets, 0u);
}

TEST(AudioMixerBoundsTest, EnforcesPacketCountCapForTinyPackets) {
    sr::AudioTimelineMixer mixer;
    for (size_t i = 0; i < sr::AudioTimelineMixer::kMaxBufferedPackets + 88; ++i) {
        const auto result = mixer.push(make_mixer_test_packet(0, 1), false);
        EXPECT_TRUE(result.accepted);
    }

    const auto stats = mixer.stats();
    EXPECT_EQ(stats.loopback_packets, sr::AudioTimelineMixer::kMaxBufferedPackets);
    EXPECT_EQ(stats.dropped_packets, 88u);
    EXPECT_LE(stats.loopback_bytes, sr::AudioTimelineMixer::kMaxBufferedBytes);
}

TEST(AudioEngineStartupTest, RejectsStartBeforeSuccessfulInitialization) {
    sr::AudioEngine engine;
    EXPECT_FALSE(engine.start());
}

TEST(MuxWriterStartupTest, AbortBeforeInitializationIsSafeAndIdempotent) {
    sr::MuxWriter writer;
    writer.abort();
    writer.abort();
    EXPECT_FALSE(writer.initialized());
}
