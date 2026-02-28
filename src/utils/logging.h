#pragma once
// logging.h — Minimal logging utility for ScreenRecorder
// Uses OutputDebugString for debug builds, can be extended to file logging

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdarg>

namespace sr {

enum class LogLevel { Debug, Info, Warn, Error };

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

#ifdef _DEBUG
    fwprintf(stderr, L"%s", out);
#endif
}

#define SR_LOG_DEBUG(fmt, ...) sr::log(sr::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define SR_LOG_INFO(fmt, ...)  sr::log(sr::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define SR_LOG_WARN(fmt, ...)  sr::log(sr::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define SR_LOG_ERROR(fmt, ...) sr::log(sr::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace sr
