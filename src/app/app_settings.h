#pragma once
// app_settings.h — Application-level settings with INI file persistence
// T025/T026/T027: FPS preset (30/60), output directory, persisted across restarts
// Stored at: %APPDATA%\ScreenRecorder\settings.ini

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <filesystem>
#include "utils/logging.h"

namespace sr {

struct AppSettings {
    // Video settings (T026)
    uint32_t     fps         = 30;           // 30 or 60
    uint32_t     bitrate_bps = 8'000'000;    // auto-selected based on fps

    // Storage settings (T025)
    std::wstring output_dir;                 // empty = use Videos\Recordings default

    // --------------------------------------------------------------------------
    // Load from %APPDATA%\ScreenRecorder\settings.ini
    // Returns false only on hard failure; missing file is treated as "use defaults"
    bool load() {
        std::wstring ini = ini_path();
        if (ini.empty()) return false;

        // FPS
        fps = static_cast<uint32_t>(
            GetPrivateProfileIntW(L"Video", L"fps", 30, ini.c_str()));
        if (fps != 30 && fps != 60) fps = 30;  // enforce valid values

        // Auto-assign bitrate based on fps
        bitrate_bps = (fps == 60) ? 14'000'000 : 8'000'000;

        // Output directory
        wchar_t buf[MAX_PATH]{};
        GetPrivateProfileStringW(L"Storage", L"output_dir", L"",
                                 buf, MAX_PATH, ini.c_str());
        output_dir = buf;

        SR_LOG_INFO(L"Settings loaded: fps=%u, output_dir=%s",
                    fps, output_dir.empty() ? L"(default)" : output_dir.c_str());
        return true;
    }

    // --------------------------------------------------------------------------
    // Save to INI
    bool save() const {
        std::wstring ini = ini_path();
        if (ini.empty()) return false;

        // Ensure parent directory exists
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(ini).parent_path(), ec);
        if (ec) {
            SR_LOG_ERROR(L"Cannot create settings directory");
            return false;
        }

        wchar_t buf[16];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u", fps);
        WritePrivateProfileStringW(L"Video",   L"fps",        buf,             ini.c_str());
        WritePrivateProfileStringW(L"Storage", L"output_dir", output_dir.c_str(), ini.c_str());

        SR_LOG_INFO(L"Settings saved: fps=%u, output_dir=%s",
                    fps, output_dir.empty() ? L"(default)" : output_dir.c_str());
        return true;
    }

    // --------------------------------------------------------------------------
    // Path to INI file: %APPDATA%\ScreenRecorder\settings.ini
    static std::wstring ini_path() {
        wchar_t appdata[MAX_PATH]{};
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            // Fallback
            GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        }
        if (appdata[0] == L'\0') return {};
        std::wstring path = std::wstring(appdata) + L"\\ScreenRecorder\\settings.ini";
        return path;
    }
};

} // namespace sr
