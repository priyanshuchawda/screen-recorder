#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

namespace sr {

inline std::wstring format_dxgi_memory_fields(bool available, uint64_t age_ms,
                                              uint64_t local_usage_bytes,
                                              uint64_t local_budget_bytes,
                                              uint64_t nonlocal_usage_bytes,
                                              uint64_t nonlocal_budget_bytes) {
    wchar_t fields[384]{};
    _snwprintf_s(fields, _countof(fields), _TRUNCATE,
        L"gpu_memory_scope=process_dxgi gpu_memory_available=%u gpu_memory_age_ms=%llu "
        L"gpu_local_usage_bytes=%llu gpu_local_budget_bytes=%llu "
        L"gpu_nonlocal_usage_bytes=%llu gpu_nonlocal_budget_bytes=%llu",
        available ? 1u : 0u,
        static_cast<unsigned long long>(age_ms),
        static_cast<unsigned long long>(local_usage_bytes),
        static_cast<unsigned long long>(local_budget_bytes),
        static_cast<unsigned long long>(nonlocal_usage_bytes),
        static_cast<unsigned long long>(nonlocal_budget_bytes));
    return fields;
}

} // namespace sr
