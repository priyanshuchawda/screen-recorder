#include <gtest/gtest.h>

#include "utils/session_diagnostics.h"

TEST(SessionDiagnosticsTest, PathForOutputReplacesMp4Suffix) {
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(
            L"C:\\Recordings\\ScreenRec_2026-05-25_20-00-00.mp4"),
        L"C:\\Recordings\\_diagnostics\\ScreenRec_2026-05-25_20-00-00.diagnostics.txt");
}

TEST(SessionDiagnosticsTest, PathForOutputAppendsWhenSuffixIsDifferent) {
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(L"C:\\Recordings\\ScreenRec.partial.mp4"),
        L"C:\\Recordings\\_diagnostics\\ScreenRec.partial.diagnostics.txt");
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(L"C:\\Recordings\\ScreenRec"),
        L"C:\\Recordings\\_diagnostics\\ScreenRec.diagnostics.txt");
}

TEST(SessionDiagnosticsTest, StartSummaryIncludesHardwareProfileAndPowerState) {
    sr::SessionDiagnostics::StartInfo info;
    info.build_commit = L"abc123def456";
    info.output_path = L"C:\\Recordings\\ScreenRec.mp4";
    info.adapter_name = L"Intel(R) Iris(R) Xe Graphics";
    info.probed_encoder_name = L"Intel Quick Sync Video H.264 Encoder MFT";
    info.encoder_mode = L"HW";
    info.power_state = L"Battery";
    info.high_quality = true;
    info.width = 1920;
    info.height = 1080;
    info.fps = 60;
    info.bitrate_bps = 10'000'000;

    const std::wstring summary = sr::SessionDiagnostics::format_start_summary(info);

    EXPECT_NE(summary.find(L"output=C:\\Recordings\\ScreenRec.mp4"), std::wstring::npos);
    EXPECT_NE(summary.find(L"build_commit=abc123def456"), std::wstring::npos);
    EXPECT_NE(summary.find(L"adapter=Intel(R) Iris(R) Xe Graphics"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_mode=HW"), std::wstring::npos);
    EXPECT_NE(summary.find(L"power=Battery"), std::wstring::npos);
    EXPECT_NE(summary.find(L"quality=HQ"), std::wstring::npos);
    EXPECT_NE(summary.find(L"profile=1920x1080@60fps 10000000bps"), std::wstring::npos);
}

TEST(SessionDiagnosticsTest, StopSummaryMarksIncompleteEncoderDrain) {
    sr::SessionDiagnostics::StopInfo info;
    info.status = L"incomplete";
    info.encoder_mode = L"HW";
    info.encoder_drain_complete = false;
    info.encoder_pending_inputs = 4;
    info.encoder_drain_samples = 2;
    info.encoder_drain_ms = 3000;
    info.finalize_ms = 45;
    info.encoder_inputs_abandoned = 3;
    info.encoder_inputs_offered = 12;
    info.encoder_inputs_accepted = 10;
    info.encoder_outputs_produced = 9;
    info.encoder_inputs_retried = 2;
    info.encoder_input_backpressure_attempts = 4;
    info.encoder_input_backpressure_timeouts = 1;
    info.encoder_input_backpressure_recoveries = 2;
    info.encoder_input_backpressure_cancellations = 1;
    info.encoder_input_backpressure_max_wait_us = 1200;
    info.encoder_input_backpressure_p95_wait_us = 1024;
    info.audio_write_failures = 1;
    info.audio_source_start_failures = 2;
    info.diagnostic_log_dropped = 2;

    const std::wstring summary = sr::SessionDiagnostics::format_stop_summary(info);
    EXPECT_NE(summary.find(L"status=incomplete"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_drain_complete=0"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_pending_inputs=4"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_drain_ms=3000"), std::wstring::npos);
    EXPECT_NE(summary.find(L"finalize_ms=45"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_inputs_abandoned=3"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_inputs_offered=12"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_inputs_accepted=10"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_outputs_produced=9"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_inputs_retried=2"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_attempts=4"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_timeouts=1"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_recoveries=2"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_cancellations=1"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_max_wait_us=1200"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_input_backpressure_p95_wait_us=1024"), std::wstring::npos);
    EXPECT_NE(summary.find(L"audio_write_failures=1"), std::wstring::npos);
    EXPECT_NE(summary.find(L"audio_source_start_failures=2"), std::wstring::npos);
    EXPECT_NE(summary.find(L"diagnostic_log_dropped=2"), std::wstring::npos);
}

TEST(SessionDiagnosticsTest, StopSummaryIncludesCaptureSetupFailures) {
    sr::SessionDiagnostics::StopInfo info;
    info.status = L"incomplete";
    info.capture_resize_failures = 2;
    info.capture_texture_acquisition_failures = 3;
    const std::wstring summary = sr::SessionDiagnostics::format_stop_summary(info);
    EXPECT_NE(summary.find(L"capture_resize_failures=2"), std::wstring::npos);
    EXPECT_NE(summary.find(L"capture_texture_acquisition_failures=3"), std::wstring::npos);
}

TEST(SessionDiagnosticsTest, StopSummaryReportsMissedFixedFrameSlots) {
    sr::SessionDiagnostics::StopInfo info;
    info.status = L"incomplete";
    info.video_scheduled_frame_drops = 3;
    const std::wstring summary = sr::SessionDiagnostics::format_stop_summary(info);
    EXPECT_NE(summary.find(L"video_scheduled_frame_drops=3"), std::wstring::npos);
}
