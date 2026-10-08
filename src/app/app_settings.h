#pragma once
// Application settings stored at %APPDATA%\ScreenRecorder\settings.ini.

#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <filesystem>
#include <string>

#include "utils/logging.h"
#include "utils/render_frame.h"

namespace sr {

struct AppSettings {
    uint32_t fps = 30;
    uint32_t resolution_height = 480;
    uint32_t bitrate_bps = 4'000'000;
    std::wstring output_dir;
    bool camera_overlay_enabled = false;

    bool is_high_quality() const noexcept { return resolution_height >= 720; }

    bool load() {
        const std::wstring ini = ini_path();
        if (ini.empty()) return false;

        fps = static_cast<uint32_t>(GetPrivateProfileIntW(L"Video", L"fps", 30, ini.c_str()));
        if (fps != 30 && fps != 60) fps = 30;

        const uint32_t stored_height = static_cast<uint32_t>(
            GetPrivateProfileIntW(L"Video", L"resolution_height", 0, ini.c_str()));
        if (is_supported_resolution(stored_height)) {
            resolution_height = stored_height;
        } else {
            // Migrate settings from releases that stored only a High Quality flag.
            resolution_height = GetPrivateProfileIntW(
                L"Video", L"high_quality", 0, ini.c_str()) != 0 ? 1080u : 480u;
        }
        bitrate_bps = compute_bitrate(fps, resolution_height);

        wchar_t buf[MAX_PATH]{};
        GetPrivateProfileStringW(L"Storage", L"output_dir", L"", buf, MAX_PATH, ini.c_str());
        output_dir = buf;
        camera_overlay_enabled = GetPrivateProfileIntW(
            L"Camera", L"overlay_enabled", 0, ini.c_str()) != 0;

        SR_LOG_INFO(L"Settings loaded: fps=%u, resolution=%up, bitrate=%u, output_dir=%s, camera_overlay=%s",
                    fps, resolution_height, bitrate_bps,
                    output_dir.empty() ? L"(default)" : output_dir.c_str(),
                    camera_overlay_enabled ? L"on" : L"off");
        return true;
    }

    bool save() const {
        const std::wstring ini = ini_path();
        if (ini.empty()) return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(ini).parent_path(), ec);
        if (ec) {
            SR_LOG_ERROR(L"Cannot create settings directory");
            return false;
        }

        wchar_t buf[16]{};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u", fps);
        WritePrivateProfileStringW(L"Video", L"fps", buf, ini.c_str());
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u", resolution_height);
        WritePrivateProfileStringW(L"Video", L"resolution_height", buf, ini.c_str());
        WritePrivateProfileStringW(L"Storage", L"output_dir", output_dir.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Camera", L"overlay_enabled",
                                   camera_overlay_enabled ? L"1" : L"0", ini.c_str());

        SR_LOG_INFO(L"Settings saved: fps=%u, resolution=%up, output_dir=%s, camera_overlay=%s",
                    fps, resolution_height,
                    output_dir.empty() ? L"(default)" : output_dir.c_str(),
                    camera_overlay_enabled ? L"on" : L"off");
        return true;
    }

    static std::wstring ini_path() {
        wchar_t appdata[MAX_PATH]{};
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        }
        if (appdata[0] == L'\0') return {};
        return std::wstring(appdata) + L"\\ScreenRecorder\\settings.ini";
    }

    static bool is_supported_resolution(uint32_t height) noexcept {
        return height == 360 || height == 480 || height == 720 || height == 1080;
    }

    static uint32_t compute_bitrate(uint32_t fps, uint32_t height) noexcept {
        const bool sixty_fps = fps == 60;
        switch (height) {
            case 360: return sixty_fps ? 2'500'000 : 1'500'000;
            case 720: return sixty_fps ? 8'000'000 : 5'000'000;
            case 1080: return sixty_fps ? 10'000'000 : 8'000'000;
            case 480:
            default: return sixty_fps ? 6'000'000 : 4'000'000;
        }
    }

    void set_resolution(uint32_t height) noexcept {
        if (!is_supported_resolution(height)) return;
        resolution_height = height;
        bitrate_bps = compute_bitrate(fps, resolution_height);
    }
};

} // namespace sr
