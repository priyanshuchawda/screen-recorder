#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#ifdef SR_LOCAL_RECORDING_PROBE
#include <windows.h>
#endif

namespace sr {

#ifdef SR_LOCAL_RECORDING_PROBE
inline std::atomic<bool>& probe_audio_stall_triggered_state() noexcept {
    static std::atomic<bool> triggered{false};
    return triggered;
}

inline uint64_t probe_audio_stall_after_writes() noexcept {
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(L"SR_PROBE_STALL_AUDIO_AFTER_WRITES", value,
                                                  static_cast<DWORD>(sizeof(value) / sizeof(value[0])));
    if (length == 0 || length >= sizeof(value) / sizeof(value[0])) return 1;
    wchar_t* end = nullptr;
    const unsigned long long parsed = _wcstoui64(value, &end, 10);
    return (end && *end == L'\0' && parsed > 0) ? parsed : 1;
}

inline uint32_t probe_audio_stall_duration_ms() noexcept {
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(L"SR_PROBE_STALL_AUDIO_MS", value,
                                                  static_cast<DWORD>(sizeof(value) / sizeof(value[0])));
    if (length == 0 || length >= sizeof(value) / sizeof(value[0])) return 18'000;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 10);
    if (!end || *end != L'\0' || parsed == 0) return 18'000;
    return static_cast<uint32_t>((parsed > 120'000UL) ? 120'000UL : parsed);
}

inline bool try_trigger_probe_audio_stall(uint64_t audio_write_ordinal) noexcept {
    wchar_t value[2]{};
    if (GetEnvironmentVariableW(L"SR_PROBE_STALL_AUDIO", value, 2) != 1 ||
        value[0] != L'1' || audio_write_ordinal < probe_audio_stall_after_writes()) {
        return false;
    }
    bool expected = false;
    return probe_audio_stall_triggered_state().compare_exchange_strong(
        expected, true, std::memory_order_acq_rel);
}

inline bool probe_audio_stall_was_triggered() noexcept {
    return probe_audio_stall_triggered_state().load(std::memory_order_acquire);
}
#else
inline bool try_trigger_probe_audio_stall(uint64_t) noexcept { return false; }
inline bool probe_audio_stall_was_triggered() noexcept { return false; }
inline uint32_t probe_audio_stall_duration_ms() noexcept { return 0; }
#endif

} // namespace sr
