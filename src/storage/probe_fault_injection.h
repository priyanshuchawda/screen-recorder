#pragma once

#include <atomic>

#ifdef SR_LOCAL_RECORDING_PROBE
#include <windows.h>
#endif

namespace sr {

#ifdef SR_LOCAL_RECORDING_PROBE
inline std::atomic<bool>& probe_audio_stall_triggered_state() noexcept {
    static std::atomic<bool> triggered{false};
    return triggered;
}

inline bool try_trigger_probe_audio_stall() noexcept {
    wchar_t value[2]{};
    if (GetEnvironmentVariableW(L"SR_PROBE_STALL_AUDIO", value, 2) != 1 ||
        value[0] != L'1') {
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
inline bool try_trigger_probe_audio_stall() noexcept { return false; }
inline bool probe_audio_stall_was_triggered() noexcept { return false; }
#endif

} // namespace sr
