#pragma once

#include <windows.h>
#include <string>

#include "storage/storage_manager.h"

namespace sr {

struct RecoveryActionResult {
    bool succeeded = false;
    DWORD error_code = 0;
    std::wstring final_path;
};

inline RecoveryActionResult recover_orphan_partial(const std::wstring& partial_path) {
    RecoveryActionResult result;
    result.final_path = StorageManager::partialToFinal(partial_path);

    if (MoveFileExW(partial_path.c_str(),
                    result.final_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING)) {
        result.succeeded = true;
        return result;
    }

    result.error_code = GetLastError();
    return result;
}

inline RecoveryActionResult delete_orphan_partial(const std::wstring& partial_path) {
    RecoveryActionResult result;

    if (DeleteFileW(partial_path.c_str())) {
        result.succeeded = true;
        return result;
    }

    result.error_code = GetLastError();
    return result;
}

} // namespace sr
