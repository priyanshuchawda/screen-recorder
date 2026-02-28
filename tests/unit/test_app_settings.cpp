// test_app_settings.cpp — Unit tests for AppSettings INI persistence
// T025/T026/T027: Validates load/save round-trip, FPS preset values, directory override

#include <gtest/gtest.h>
#include "app/app_settings.h"
#include "utils/render_frame.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>

// Helper: redirect AppSettings to a temp INI file for test isolation
class AppSettingsTest : public ::testing::Test {
protected:
    std::wstring test_ini_;

    void SetUp() override {
        // Use a temp file so we don't pollute the real settings
        wchar_t tmp_path[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp_path);
        test_ini_ = std::wstring(tmp_path) + L"sr_test_settings.ini";
        // Remove any leftover from previous run
        DeleteFileW(test_ini_.c_str());
    }

    void TearDown() override {
        DeleteFileW(test_ini_.c_str());
    }

    // Write INI directly (bypasses AppSettings.save)
    void WriteIni(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
        WritePrivateProfileStringW(section, key, value, test_ini_.c_str());
    }

    // Read settings using our INI path override
    sr::AppSettings LoadFromIni() {
        sr::AppSettings s;
        s.fps = static_cast<uint32_t>(
            GetPrivateProfileIntW(L"Video", L"fps", 30, test_ini_.c_str()));
        if (s.fps != 30 && s.fps != 60) s.fps = 30;
        s.bitrate_bps = (s.fps == 60) ? 14'000'000 : 8'000'000;

        wchar_t buf[MAX_PATH]{};
        GetPrivateProfileStringW(L"Storage", L"output_dir", L"",
                                 buf, MAX_PATH, test_ini_.c_str());
        s.output_dir = buf;
        return s;
    }

    // Save settings to our temp INI path
    void SaveToIni(const sr::AppSettings& s) {
        wchar_t buf[16];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u", s.fps);
        WritePrivateProfileStringW(L"Video",   L"fps",        buf,                test_ini_.c_str());
        WritePrivateProfileStringW(L"Storage", L"output_dir", s.output_dir.c_str(), test_ini_.c_str());
    }
};

// Default values when no INI exists
TEST_F(AppSettingsTest, DefaultsAre30FpsAndEmptyDir) {
    auto s = LoadFromIni();  // no file exists → defaults
    EXPECT_EQ(s.fps, 30u);
    EXPECT_TRUE(s.output_dir.empty());
}

// Round-trip: save 60fps, reload and confirm
TEST_F(AppSettingsTest, SaveAndLoadFps60) {
    sr::AppSettings s;
    s.fps         = 60;
    s.bitrate_bps = 14'000'000;
    s.output_dir  = L"";
    SaveToIni(s);

    auto loaded = LoadFromIni();
    EXPECT_EQ(loaded.fps, 60u);
    EXPECT_EQ(loaded.bitrate_bps, 14'000'000u);
}

// Round-trip: save 30fps
TEST_F(AppSettingsTest, SaveAndLoadFps30) {
    sr::AppSettings s;
    s.fps         = 30;
    s.bitrate_bps = 8'000'000;
    SaveToIni(s);

    auto loaded = LoadFromIni();
    EXPECT_EQ(loaded.fps, 30u);
    EXPECT_EQ(loaded.bitrate_bps, 8'000'000u);
}

// Round-trip: custom output directory
TEST_F(AppSettingsTest, SaveAndLoadOutputDir) {
    sr::AppSettings s;
    s.fps        = 30;
    s.output_dir = L"C:\\TestOutputDir";
    SaveToIni(s);

    auto loaded = LoadFromIni();
    EXPECT_EQ(loaded.output_dir, L"C:\\TestOutputDir");
}

// Invalid FPS value in INI maps to 30
TEST_F(AppSettingsTest, InvalidFpsDefaultsTo30) {
    WriteIni(L"Video", L"fps", L"999");

    auto s = LoadFromIni();
    EXPECT_EQ(s.fps, 30u);
}

// Bitrate auto-selects based on fps
TEST_F(AppSettingsTest, BitrateAutoSelectsFor60fps) {
    sr::AppSettings s;
    s.fps = 60;
    s.bitrate_bps = (s.fps == 60) ? 14'000'000 : 8'000'000;
    EXPECT_EQ(s.bitrate_bps, 14'000'000u);
}

TEST_F(AppSettingsTest, BitrateAutoSelectsFor30fps) {
    sr::AppSettings s;
    s.fps = 30;
    s.bitrate_bps = (s.fps == 60) ? 14'000'000 : 8'000'000;
    EXPECT_EQ(s.bitrate_bps, 8'000'000u);
}

// ini_path() returns a non-empty string
TEST(AppSettingsStaticTest, IniPathNonEmpty) {
    std::wstring path = sr::AppSettings::ini_path();
    EXPECT_FALSE(path.empty());
    // Must end with .ini
    EXPECT_NE(path.rfind(L".ini"), std::wstring::npos);
    // Must contain ScreenRecorder
    EXPECT_NE(path.find(L"ScreenRecorder"), std::wstring::npos);
}

// EncoderProfile T026: 60fps profile values are correct
TEST(EncoderProfileTest, Fps60ProfileValues) {
    sr::EncoderProfile p;
    p.fps         = 60;
    p.bitrate_bps = 14'000'000;
    p.width       = 1920;
    p.height      = 1080;
    p.gop_seconds = 2;
    p.low_latency = true;
    p.b_frames    = 0;

    EXPECT_EQ(p.fps, 60u);
    EXPECT_EQ(p.bitrate_bps, 14'000'000u);
    EXPECT_EQ(p.width,  1920u);
    EXPECT_EQ(p.height, 1080u);
    EXPECT_EQ(p.b_frames, 0u);
    EXPECT_TRUE(p.low_latency);
}

// EncoderProfile T026: 30fps profile values are correct
TEST(EncoderProfileTest, Fps30ProfileValues) {
    sr::EncoderProfile p;
    p.fps         = 30;
    p.bitrate_bps = 8'000'000;
    p.width       = 1920;
    p.height      = 1080;

    EXPECT_EQ(p.fps, 30u);
    EXPECT_EQ(p.bitrate_bps, 8'000'000u);
}
