#include <gtest/gtest.h>

#include "utils/session_diagnostics.h"

TEST(SessionDiagnosticsTest, PathForOutputReplacesMp4Suffix) {
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(
            L"C:\\Recordings\\ScreenRec_2026-05-25_20-00-00.mp4"),
        L"C:\\Recordings\\ScreenRec_2026-05-25_20-00-00.diagnostics.txt");
}

TEST(SessionDiagnosticsTest, PathForOutputAppendsWhenSuffixIsDifferent) {
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(L"C:\\Recordings\\ScreenRec.partial.mp4"),
        L"C:\\Recordings\\ScreenRec.partial.diagnostics.txt");
    EXPECT_EQ(
        sr::SessionDiagnostics::path_for_output(L"C:\\Recordings\\ScreenRec"),
        L"C:\\Recordings\\ScreenRec.diagnostics.txt");
}

TEST(SessionDiagnosticsTest, StartSummaryIncludesHardwareProfileAndPowerState) {
    sr::SessionDiagnostics::StartInfo info;
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
    EXPECT_NE(summary.find(L"adapter=Intel(R) Iris(R) Xe Graphics"), std::wstring::npos);
    EXPECT_NE(summary.find(L"encoder_mode=HW"), std::wstring::npos);
    EXPECT_NE(summary.find(L"power=Battery"), std::wstring::npos);
    EXPECT_NE(summary.find(L"quality=HQ"), std::wstring::npos);
    EXPECT_NE(summary.find(L"profile=1920x1080@60fps 10000000bps"), std::wstring::npos);
}
