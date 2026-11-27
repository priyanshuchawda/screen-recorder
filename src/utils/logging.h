#pragma once
// logging.h — Debug output plus a bounded asynchronous session-scoped UTF-8 log.

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace sr {

enum class LogLevel { Debug, Info, Warn, Error };

inline std::mutex& persistent_log_mutex() {
    static std::mutex value;
    return value;
}

inline std::wstring& persistent_log_path() {
    static std::wstring value;
    return value;
}

inline bool write_persistent_log_line(const std::wstring& path,
                                      const std::wstring& line) {
    if (path.empty()) return false;
    std::wstring timestamped = line;
    while (!timestamped.empty() &&
           (timestamped.back() == L'\r' || timestamped.back() == L'\n')) {
        timestamped.pop_back();
    }
    timestamped += L"\r\n";

    const int required = WideCharToMultiByte(CP_UTF8, 0, timestamped.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 1) return false;
    std::string utf8(static_cast<size_t>(required), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, 0, timestamped.c_str(), -1,
                                               utf8.data(), required, nullptr, nullptr);
    if (converted <= 1) {
        return false;
    }
    utf8.resize(static_cast<size_t>(converted - 1)); // omit the terminating NUL

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == utf8.size();
}

class PersistentLogWriter {
public:
    PersistentLogWriter() : worker_([this] { run(); }) {}
    PersistentLogWriter(const PersistentLogWriter&) = delete;
    PersistentLogWriter& operator=(const PersistentLogWriter&) = delete;

    void enqueue(const std::wstring& path, const std::wstring& line) {
        if (path.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= kCapacity) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            try {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                wchar_t timestamped[4096]{};
                _snwprintf_s(timestamped, _countof(timestamped), _TRUNCATE,
                             L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s",
                             now.wYear, now.wMonth, now.wDay,
                             now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                             line.c_str());
                queue_.push_back({ path, timestamped });
            } catch (...) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        ready_.notify_one();
    }

    bool flush(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return drained_.wait_for(lock, timeout, [this] { return queue_.empty() && !writing_; });
    }

    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Entry { std::wstring path; std::wstring line; };
    static constexpr size_t kCapacity = 2048;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable drained_;
    std::deque<Entry> queue_;
    std::thread worker_;
    std::atomic<uint64_t> dropped_{ 0 };
    bool writing_ = false;

    void run() {
        for (;;) {
            Entry entry;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !queue_.empty(); });
                entry = std::move(queue_.front());
                queue_.pop_front();
                writing_ = true;
            }
            try {
                if (!write_persistent_log_line(entry.path, entry.line)) {
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                writing_ = false;
                if (queue_.empty()) drained_.notify_all();
            }
        }
    }
};

inline PersistentLogWriter& persistent_log_writer() {
    // Intentionally process-lifetime: a blocked storage driver must not make
    // static destruction wait on the diagnostic writer during app shutdown.
    static PersistentLogWriter* writer = new PersistentLogWriter();
    return *writer;
}

inline void set_persistent_log_path(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(persistent_log_mutex());
    persistent_log_path() = path;
}

inline void clear_persistent_log_path() {
    set_persistent_log_path(L"");
}

inline void append_persistent_log_line(const std::wstring& path,
                                       const std::wstring& line) {
    persistent_log_writer().enqueue(path, line);
}

inline bool flush_persistent_log(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    return persistent_log_writer().flush(timeout);
}

inline uint64_t persistent_log_dropped_count() noexcept {
    return persistent_log_writer().dropped();
}

inline const wchar_t* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO ";
        case LogLevel::Warn:  return L"WARN ";
        case LogLevel::Error: return L"ERROR";
        default:              return L"?????";
    }
}

inline void log(LogLevel level, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf) - 1, _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t out[1100];
    _snwprintf_s(out, _countof(out) - 1, _TRUNCATE,
                 L"[SR][%s] %s\n", level_str(level), buf);

    OutputDebugStringW(out);

    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(persistent_log_mutex());
        path = persistent_log_path();
    }
    persistent_log_writer().enqueue(path, out);

#ifdef _DEBUG
    fwprintf(stderr, L"%s", out);
#endif
}

#define SR_LOG_DEBUG(fmt, ...) sr::log(sr::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define SR_LOG_INFO(fmt, ...)  sr::log(sr::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define SR_LOG_WARN(fmt, ...)  sr::log(sr::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define SR_LOG_ERROR(fmt, ...) sr::log(sr::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace sr
