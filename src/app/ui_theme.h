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
inline constexpr COLORREF kRecordingHot = RGB(82, 235, 180);
inline constexpr COLORREF kPaused = RGB(245, 179, 71);
inline constexpr COLORREF kStopping = RGB(248, 113, 113);
inline constexpr COLORREF kStatusIdle = RGB(86, 99, 116);
inline constexpr COLORREF kOverlayChrome = RGB(12, 15, 18);

inline constexpr int kButtonCornerRadius = 6;
inline constexpr int kButtonFocusInset = 3;
inline constexpr int kOverlayCloseInset = 8;
inline constexpr int kOverlayCloseSize = 24;
inline constexpr int kOverlayCameraActionWidth = 196;
inline constexpr int kOverlayCameraActionHeight = 30;
inline constexpr int kOverlayCameraActionGap = 12;
inline constexpr int kMainWindowMinWidth = 640;
inline constexpr int kMainWindowMinHeight = 360;

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

enum class StatusTone {
    Idle,
    Recording,
    Paused,
    Stopping
};

struct StatusVisual {
    COLORREF accent;
    COLORREF fill;
    COLORREF border;
    COLORREF text;
    bool animated;
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

constexpr StatusVisual status_visual(StatusTone tone,
                                     bool motion_enabled) noexcept {
    switch (tone) {
    case StatusTone::Recording:
        return {
            kRecording,
            kSurface,
            kRecording,
            kTextStrong,
            motion_enabled
        };
    case StatusTone::Paused:
        return { kPaused, kSurface, kPaused, kTextStrong, false };
    case StatusTone::Stopping:
        return { kStopping, kSurface, kStopping, kTextStrong, false };
    case StatusTone::Idle:
        break;
    }

    return { kStatusIdle, kSurface, kBorder, kTextMuted, false };
}

constexpr COLORREF status_pulse_color(StatusTone tone,
                                      bool pulse_on,
                                      bool motion_enabled) noexcept {
    const auto visual = status_visual(tone, motion_enabled);
    if (visual.animated && pulse_on) {
        return kRecordingHot;
    }
    return visual.accent;
}

constexpr RECT overlay_close_rect(const RECT& bounds) noexcept {
    return {
        bounds.right - kOverlayCloseInset - kOverlayCloseSize,
        bounds.top + kOverlayCloseInset,
        bounds.right - kOverlayCloseInset,
        bounds.top + kOverlayCloseInset + kOverlayCloseSize
    };
}

constexpr int clamp_int(int value, int min_value, int max_value) noexcept {
    return value < min_value ? min_value :
           value > max_value ? max_value :
           value;
}

constexpr RECT overlay_camera_action_rect(const RECT& content_bounds) noexcept {
    const int content_w = content_bounds.right - content_bounds.left;
    const int content_h = content_bounds.bottom - content_bounds.top;
    const int max_w = content_w > 24 ? content_w - 24 : content_w;
    const int button_w = max_w < kOverlayCameraActionWidth
        ? (max_w > 0 ? max_w : 0)
        : kOverlayCameraActionWidth;
    const int button_h = content_h < kOverlayCameraActionHeight
        ? (content_h > 0 ? content_h : 0)
        : kOverlayCameraActionHeight;
    const int center_x = content_bounds.left + (content_w / 2);
    const int desired_top = content_bounds.top + (content_h / 2) +
                            kOverlayCameraActionGap;
    const int top = clamp_int(desired_top,
                              content_bounds.top,
                              content_bounds.bottom - button_h);
    return {
        center_x - (button_w / 2),
        top,
        center_x - (button_w / 2) + button_w,
        top + button_h
    };
}

constexpr bool point_in_rect(const RECT& bounds, int x, int y) noexcept {
    return x >= bounds.left && x < bounds.right &&
           y >= bounds.top && y < bounds.bottom;
}

}  // namespace sr::ui
