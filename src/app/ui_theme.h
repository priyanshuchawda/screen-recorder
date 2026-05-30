#pragma once

#include <windows.h>

namespace sr::ui {

inline constexpr COLORREF kWindowBackground = RGB(13, 15, 18);
inline constexpr COLORREF kHeaderBackground = RGB(17, 20, 24);
inline constexpr COLORREF kSurface = RGB(23, 27, 32);
inline constexpr COLORREF kSurfaceRaised = RGB(34, 39, 46);
inline constexpr COLORREF kSurfaceHot = RGB(42, 49, 58);
inline constexpr COLORREF kSurfacePressed = RGB(27, 32, 39);
inline constexpr COLORREF kSurfaceDisabled = RGB(47, 51, 58);
inline constexpr COLORREF kBorder = RGB(58, 66, 78);
inline constexpr COLORREF kBorderStrong = RGB(86, 99, 116);
inline constexpr COLORREF kFocusRing = RGB(120, 210, 255);
inline constexpr COLORREF kTextStrong = RGB(248, 250, 252);
inline constexpr COLORREF kText = RGB(226, 232, 240);
inline constexpr COLORREF kTextMuted = RGB(163, 174, 190);
inline constexpr COLORREF kTextDisabled = RGB(122, 132, 146);
inline constexpr COLORREF kAccent = RGB(0, 136, 204);
inline constexpr COLORREF kAccentHot = RGB(0, 158, 230);
inline constexpr COLORREF kAccentPressed = RGB(0, 108, 176);
inline constexpr COLORREF kAccentBorder = RGB(84, 191, 245);
inline constexpr COLORREF kRecording = RGB(40, 210, 146);
inline constexpr COLORREF kOverlayChrome = RGB(12, 15, 18);

inline constexpr int kButtonCornerRadius = 6;
inline constexpr int kButtonFocusInset = 3;
inline constexpr int kOverlayCloseInset = 8;
inline constexpr int kOverlayCloseSize = 24;

enum class ButtonRole {
    Primary,
    Secondary
};

enum class ButtonInteraction {
    Normal,
    Hot,
    Pressed,
    Disabled
};

struct ButtonVisual {
    COLORREF fill;
    COLORREF border;
    COLORREF text;
};

constexpr ButtonVisual button_visual(ButtonRole role,
                                     ButtonInteraction interaction) noexcept {
    if (interaction == ButtonInteraction::Disabled) {
        return { kSurfaceDisabled, kBorder, kTextDisabled };
    }

    if (role == ButtonRole::Primary) {
        switch (interaction) {
        case ButtonInteraction::Hot:
            return { kAccentHot, kAccentBorder, kTextStrong };
        case ButtonInteraction::Pressed:
            return { kAccentPressed, kAccent, kTextStrong };
        case ButtonInteraction::Normal:
        case ButtonInteraction::Disabled:
            break;
        }
        return { kAccent, kAccentBorder, kTextStrong };
    }

    switch (interaction) {
    case ButtonInteraction::Hot:
        return { kSurfaceHot, kBorderStrong, kTextStrong };
    case ButtonInteraction::Pressed:
        return { kSurfacePressed, kBorderStrong, kText };
    case ButtonInteraction::Normal:
    case ButtonInteraction::Disabled:
        break;
    }

    return { kSurfaceRaised, kBorder, kText };
}

constexpr RECT overlay_close_rect(const RECT& bounds) noexcept {
    return {
        bounds.right - kOverlayCloseInset - kOverlayCloseSize,
        bounds.top + kOverlayCloseInset,
        bounds.right - kOverlayCloseInset,
        bounds.top + kOverlayCloseInset + kOverlayCloseSize
    };
}

constexpr bool point_in_rect(const RECT& bounds, int x, int y) noexcept {
    return x >= bounds.left && x < bounds.right &&
           y >= bounds.top && y < bounds.bottom;
}

}  // namespace sr::ui
