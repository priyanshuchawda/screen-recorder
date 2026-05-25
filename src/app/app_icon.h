#pragma once

#include <windows.h>

namespace sr {

inline constexpr int kAppIconResourceId = 1;

inline LPCWSTR app_icon_resource_name() noexcept {
    return MAKEINTRESOURCEW(kAppIconResourceId);
}

inline HICON load_app_icon(HINSTANCE instance,
                           int width = 0,
                           int height = 0) noexcept {
    HICON icon = reinterpret_cast<HICON>(
        LoadImageW(instance,
                   app_icon_resource_name(),
                   IMAGE_ICON,
                   width,
                   height,
                   width > 0 || height > 0 ? LR_DEFAULTCOLOR : LR_DEFAULTSIZE));
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

} // namespace sr
