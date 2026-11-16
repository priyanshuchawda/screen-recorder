#pragma once
// logging.h — Debug output plus a session-scoped persistent UTF-8 log.

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>

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

inline void append_persistent_log_line_locked(const std::wstring& path,
                                               const std::wstring& line) {
    if (path.empty()) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamped[4096]{};
    _snwprintf_s(timestamped, _countof(timestamped), _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s",
                 now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                 line.c_str());
    size_t chars = wcslen(timestamped);
    while (chars > 0 && (timestamped[chars - 1] == L'\r' || timestamped[chars - 1] == L'\n')) {
        timestamped[--chars] = L'\0';
    }
    wcscat_s(timestamped, L"\r\n");

    const int required = WideCharToMultiByte(CP_UTF8, 0, timestamped, -1,
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 1) return;
    std::string utf8(static_cast<size_t>(required), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, 0, timestamped, -1,
                                               utf8.data(), required, nullptr, nullptr);
    if (converted <= 1) {
        return;
    }
    utf8.resize(static_cast<size_t>(converted - 1)); // omit the terminating NUL

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
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
    std::lock_guard<std::mutex> lock(persistent_log_mutex());
    append_persistent_log_line_locked(path, line);
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

    {
        std::lock_guard<std::mutex> lock(persistent_log_mutex());
        append_persistent_log_line_locked(persistent_log_path(), out);
    }

#ifdef _DEBUG
    fwprintf(stderr, L"%s", out);
#endif
}

#define SR_LOG_DEBUG(fmt, ...) sr::log(sr::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define SR_LOG_INFO(fmt, ...)  sr::log(sr::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define SR_LOG_WARN(fmt, ...)  sr::log(sr::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define SR_LOG_ERROR(fmt, ...) sr::log(sr::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace sr
