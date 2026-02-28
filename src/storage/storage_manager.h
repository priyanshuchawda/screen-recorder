#pragma once
// storage_manager.h — Manages output directory, unique filenames, disk space, and file locking

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include "utils/logging.h"

namespace sr {

using DiskSpaceLowCallback = std::function<void()>;

class StorageManager {
public:
    StorageManager() {
        resolveDefaultDirectory();
    }

    ~StorageManager() {
        stopDiskSpacePolling();
    }

    // Resolve default output directory: %USERPROFILE%\Videos\Recordings
    bool resolveDefaultDirectory() {
        wchar_t* videos_path = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos_path);
        if (SUCCEEDED(hr) && videos_path) {
            output_dir_ = std::wstring(videos_path) + L"\\Recordings";
            CoTaskMemFree(videos_path);
        } else {
            // Fallback
            wchar_t profile[MAX_PATH];
            if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
                output_dir_ = std::wstring(profile) + L"\\Videos\\Recordings";
            } else {
                output_dir_ = L"C:\\Recordings";
            }
        }

        // Create directory if missing
        std::error_code ec;
        std::filesystem::create_directories(output_dir_, ec);
        if (ec) {
            SR_LOG_ERROR(L"Failed to create output dir: %s", output_dir_.c_str());
            return false;
        }
        SR_LOG_INFO(L"Output directory: %s", output_dir_.c_str());
        return true;
    }

    // Set custom output directory (validates and creates if needed)
    bool setOutputDirectory(const std::wstring& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) {
            SR_LOG_ERROR(L"Cannot set output dir: %s", path.c_str());
            return false;
        }
        output_dir_ = path;
        SR_LOG_INFO(L"Output directory changed to: %s", output_dir_.c_str());
        return true;
    }

    // Generate unique filename: ScreenRec_YYYY-MM-DD_HH-mm-ss[_NNN].partial.mp4
    std::wstring generateFilename() const {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);

        wchar_t ts[64];
        wcsftime(ts, _countof(ts), L"ScreenRec_%Y-%m-%d_%H-%M-%S", &tm_buf);

        std::wstring base = output_dir_ + L"\\" + ts;

        // Check for conflicts and add suffix if needed
        std::wstring partial = base + L".partial.mp4";
        std::wstring final_name = base + L".mp4";

        int suffix = 0;
        while (std::filesystem::exists(partial) || std::filesystem::exists(final_name)) {
            suffix++;
            wchar_t suffix_str[16];
            _snwprintf_s(suffix_str, _countof(suffix_str), _TRUNCATE, L"_%03d", suffix);
            partial = base + suffix_str + L".partial.mp4";
            final_name = base + suffix_str + L".mp4";
        }

        return partial;
    }

    // Get final path from partial path (remove ".partial" from name)
    static std::wstring partialToFinal(const std::wstring& partial_path) {
        std::wstring result = partial_path;
        size_t pos = result.rfind(L".partial.mp4");
        if (pos != std::wstring::npos) {
            result.replace(pos, 12, L".mp4");
        }
        return result;
    }

    // Check available disk space in bytes
    uint64_t getFreeDiskSpace() const {
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW(output_dir_.c_str(), &free_bytes, nullptr, nullptr)) {
            return free_bytes.QuadPart;
        }
        return 0;
    }

    // Check if disk space is critically low (< threshold_bytes, default 500 MB)
    bool isDiskSpaceLow(uint64_t threshold_bytes = 500ULL * 1024 * 1024) const {
        return getFreeDiskSpace() < threshold_bytes;
    }

    // Scan for orphaned .partial.mp4 files
    std::vector<std::wstring> findOrphanedFiles() const {
        std::vector<std::wstring> orphans;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(output_dir_, ec)) {
            if (entry.is_regular_file()) {
                auto name = entry.path().filename().wstring();
                if (name.size() > 12 && name.substr(name.size() - 12) == L".partial.mp4") {
                    orphans.push_back(entry.path().wstring());
                }
            }
        }
        return orphans;
    }

    const std::wstring& outputDirectory() const { return output_dir_; }

    // -----------------------------------------------------------------------
    // T028 — Async disk space polling
    // Fires callback on main thread (or poll thread) when free space < threshold.
    // interval_ms: polling interval (default 5000 ms)
    // threshold_bytes: low-disk threshold (default 500 MB; configurable for tests)
    // -----------------------------------------------------------------------
    void startDiskSpacePolling(DiskSpaceLowCallback callback,
                               std::chrono::milliseconds interval = std::chrono::milliseconds(5000),
                               uint64_t threshold_bytes = 500ULL * 1024 * 1024)
    {
        stopDiskSpacePolling(); // stop any existing poll thread
        low_disk_cb_  = std::move(callback);
        poll_running_.store(true, std::memory_order_release);
        poll_thread_  = std::thread([this, interval, threshold_bytes]() {
            while (poll_running_.load(std::memory_order_acquire)) {
                if (isDiskSpaceLow(threshold_bytes) && low_disk_cb_) {
                    SR_LOG_WARN(L"Disk space low! Free: %llu MB",
                                getFreeDiskSpace() / (1024 * 1024));
                    low_disk_cb_();
                }
                // Sleep in 250ms increments so stop is responsive
                auto deadline = std::chrono::steady_clock::now() + interval;
                while (poll_running_.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        });
    }

    void stopDiskSpacePolling() {
        poll_running_.store(false, std::memory_order_release);
        if (poll_thread_.joinable()) {
            if (poll_thread_.get_id() == std::this_thread::get_id()) {
                // Called re-entrantly from the poll thread (e.g. low-disk callback -> stop())
                // Detach to avoid deadlock — the thread will exit naturally
                poll_thread_.detach();
            } else {
                poll_thread_.join();
            }
        }
    }

private:
    std::wstring output_dir_;

    // T028 — polling state
    std::thread              poll_thread_;
    std::atomic<bool>        poll_running_{ false };
    DiskSpaceLowCallback     low_disk_cb_;
};

} // namespace sr
