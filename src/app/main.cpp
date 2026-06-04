// main.cpp — ScreenRecorder entry point
// T017: Minimal Win32 window wired to SessionController for Start/Stop/Pause/Mute
// T037: Debug telemetry overlay uses TelemetrySnapshot
// T043: WGC availability check disables Start button on unsupported hardware

#include <windows.h>
#include <processthreadsapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <cstdio>
#include <thread>
#include "utils/logging.h"
#include "utils/qpc_clock.h"
#include "storage/storage_manager.h"
#include "controller/session_controller.h"
#include "capture/capture_engine.h"   // T043: is_wgc_supported()
#include "app/app_settings.h"
#include "app/settings_dialog.h"
#include "app/telemetry.h"             // T037: TelemetrySnapshot
#include "app/camera_overlay.h"
#include "app/app_icon.h"
#include "app/app_version.h"
#include "app/stop_flow.h"
#include "app/ui_theme.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

// Control IDs
#define ID_BTN_START     1001
#define ID_BTN_STOP      1002
#define ID_BTN_PAUSE     1003
#define ID_BTN_MUTE      1004
#define ID_LABEL_STATUS  1005
#define ID_LABEL_TIME    1006
#define ID_LABEL_FPS     1007
#define ID_LABEL_PATH    1008
#define ID_LABEL_DROPPED 1009
#define ID_BTN_SETTINGS  1010
#define ID_LABEL_PROFILE 1011
#define ID_BTN_HQ        1012
#define ID_TIMER_UPDATE  1

// Custom messages for marshalling background thread callbacks to UI thread
#define WM_SR_STATUS  (WM_USER + 1)
#define WM_SR_ERROR   (WM_USER + 2)
#define WM_SR_STOP_COMPLETE (WM_USER + 3)

// Global state
static sr::AppSettings       g_settings;
static sr::StorageManager    g_storage;
static sr::SessionController g_controller;
static sr::CameraOverlay     g_camera_overlay;
static sr::StopFlow          g_stop_flow;
static std::thread           g_stop_thread;

static HWND g_hwnd          = nullptr;
static HWND g_btn_start     = nullptr;
static HWND g_btn_stop      = nullptr;
static HWND g_btn_pause     = nullptr;
static HWND g_btn_mute      = nullptr;
static HWND g_btn_settings  = nullptr;
static HWND g_btn_hq        = nullptr;
static HWND g_lbl_status    = nullptr;
static HWND g_lbl_time      = nullptr;
static HWND g_lbl_fps       = nullptr;
static HWND g_lbl_path      = nullptr;
static HWND g_lbl_dropped   = nullptr;
static HWND g_lbl_profile   = nullptr;
static HWND g_lbl_title     = nullptr;

static HBRUSH g_brush_bg    = nullptr;
static HBRUSH g_brush_header = nullptr;
static HFONT  g_font_ui     = nullptr;
static HFONT  g_font_bold   = nullptr;
static HFONT  g_font_title  = nullptr;
static HFONT  g_font_metric = nullptr;
static HWND   g_hot_button  = nullptr;
static bool   g_motion_enabled = true;
static sr::SessionState g_last_ui_state = sr::SessionState::Idle;

static constexpr COLORREF kBgColor     = sr::ui::kWindowBackground;
static constexpr COLORREF kTextColor   = sr::ui::kText;
static constexpr COLORREF kMutedText   = sr::ui::kTextMuted;
static constexpr COLORREF kBorderColor = sr::ui::kBorder;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static int64_t g_record_start_ms = 0;
static int64_t g_paused_total_ms = 0;
static int64_t g_pause_start_ms  = 0;

// ----------------------------------------------------------------------------
static bool IsClientAreaAnimationEnabled() noexcept
{
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != FALSE) {
        return enabled != FALSE;
    }
    return true;
}

static sr::ui::StatusTone StatusToneForState(sr::SessionState state) noexcept
{
    switch (state) {
        case sr::SessionState::Recording: return sr::ui::StatusTone::Recording;
        case sr::SessionState::Paused:    return sr::ui::StatusTone::Paused;
        case sr::SessionState::Stopping:  return sr::ui::StatusTone::Stopping;
        case sr::SessionState::Idle:      break;
    }
    return sr::ui::StatusTone::Idle;
}

static const wchar_t* StatusPillLabel(sr::SessionState state) noexcept
{
    switch (state) {
        case sr::SessionState::Recording: return L"REC";
        case sr::SessionState::Paused:    return L"PAUSED";
        case sr::SessionState::Stopping:  return L"STOPPING";
        case sr::SessionState::Idle:      break;
    }
    return L"READY";
}

static void InvalidateStatusChrome(HWND hwnd)
{
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    rc.bottom = 44;
    InvalidateRect(hwnd, &rc, FALSE);
}

static void ApplyEncoderProfileFromSettings()
{
    sr::EncoderProfile profile;
    profile.fps         = g_settings.fps;
    profile.bitrate_bps = g_settings.bitrate_bps;
    const auto resolution = sr::recording_resolution_for_quality(g_settings.high_quality);
    profile.width       = resolution.width;
    profile.height      = resolution.height;
    g_controller.set_encoder_profile(profile, g_settings.high_quality);
}

static void ApplyCameraProfileFromSettings()
{
    g_camera_overlay.set_high_quality(g_settings.high_quality);
}

static void UpdateProfileLabel()
{
    if (!g_lbl_profile) return;
    const auto resolution = sr::recording_resolution_for_quality(g_settings.high_quality);
    wchar_t prof_buf[96];
    _snwprintf_s(prof_buf, _countof(prof_buf), _TRUNCATE,
        L"%ufps | %ux%u | %uMbps%s",
        g_settings.fps,
        resolution.width, resolution.height,
        g_settings.bitrate_bps / 1'000'000,
        g_settings.high_quality ? L" | HQ" : L"");
    SetWindowTextW(g_lbl_profile, prof_buf);
}

static void LayoutMainWindow(HWND hwnd)
{
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;

    const int content_w = rc.right > 24 ? rc.right - 24 : 100;
    if (g_lbl_fps) {
        SetWindowPos(g_lbl_fps, nullptr, 12, 102, content_w, 20,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_lbl_dropped) {
        SetWindowPos(g_lbl_dropped, nullptr, 12, 126, content_w, 20,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_lbl_path) {
        SetWindowPos(g_lbl_path, nullptr, 12, 156, content_w, 20,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_lbl_profile) {
        const int x = 262;
        const int w = rc.right > x + 24 ? rc.right - x - 24 : 120;
        SetWindowPos(g_lbl_profile, nullptr, x, 242, w, 18,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void JoinStopThreadIfFinished()
{
    if (g_stop_thread.joinable()) {
        g_stop_thread.join();
    }
}

void UpdateUI();

static void BeginStopAsync(HWND hwnd, bool exit_after_stop)
{
    if (!g_stop_flow.begin_stop(exit_after_stop)) {
        UpdateUI();
        return;
    }

    JoinStopThreadIfFinished();
    SetWindowTextW(g_lbl_status, L"Stopping");
    UpdateUI();

    g_stop_thread = std::thread([hwnd]() {
        const bool stopped = g_controller.stop();
        if (!PostMessageW(hwnd, WM_SR_STOP_COMPLETE, stopped ? 1 : 0, 0)) {
            SR_LOG_WARN(L"Stop completion message could not be posted: %u", GetLastError());
        }
    });
}

void UpdateUI()
{
    const auto state = g_controller.state();
    const bool stop_in_progress = g_stop_flow.stop_in_progress();
    const auto display_state = stop_in_progress ? sr::SessionState::Stopping : state;
    const bool state_changed = (display_state != g_last_ui_state);
    g_last_ui_state = display_state;

    switch (display_state) {
        case sr::SessionState::Idle:      SetWindowTextW(g_lbl_status, L"Idle");      break;
        case sr::SessionState::Recording: SetWindowTextW(g_lbl_status, L"Recording"); break;
        case sr::SessionState::Paused:    SetWindowTextW(g_lbl_status, L"Paused");    break;
        case sr::SessionState::Stopping:  SetWindowTextW(g_lbl_status, L"Stopping");  break;
    }

    bool can_start = (state == sr::SessionState::Idle) && !stop_in_progress;
    bool can_stop  = (state == sr::SessionState::Recording || state == sr::SessionState::Paused) &&
                     !stop_in_progress;

    EnableWindow(g_btn_start, can_start ? TRUE : FALSE);
    EnableWindow(g_btn_stop,  can_stop  ? TRUE : FALSE);
    EnableWindow(g_btn_pause, can_stop  ? TRUE : FALSE);
    EnableWindow(g_btn_mute,  can_stop  ? TRUE : FALSE);
    EnableWindow(g_btn_hq,    can_start ? TRUE : FALSE);

    SetWindowTextW(g_btn_pause, (state == sr::SessionState::Paused) ? L"Resume" : L"Pause");
    SetWindowTextW(g_btn_mute,  g_controller.is_muted() ? L"Unmute" : L"Mute");
    SetWindowTextW(g_btn_hq,    g_settings.high_quality ? L"HQ On" : L"HQ Off");

    // Elapsed time
    if (display_state == sr::SessionState::Recording ||
        display_state == sr::SessionState::Paused ||
        display_state == sr::SessionState::Stopping) {
        double now_ms   = static_cast<double>(sr::QPCClock::instance().now_ms());
        int64_t elapsed = static_cast<int64_t>(now_ms) - g_record_start_ms - g_paused_total_ms;
        if (state == sr::SessionState::Paused)
            elapsed = g_pause_start_ms - g_record_start_ms - g_paused_total_ms;
        if (elapsed < 0) elapsed = 0;

        int secs = static_cast<int>(elapsed / 1000);
        int mins = secs / 60; secs %= 60;
        int hrs  = mins / 60; mins %= 60;
        wchar_t tbuf[32];
        _snwprintf_s(tbuf, _countof(tbuf), _TRUNCATE, L"%02d:%02d:%02d", hrs, mins, secs);
        SetWindowTextW(g_lbl_time, tbuf);
    } else {
        SetWindowTextW(g_lbl_time, L"00:00:00");
    }

    // Counters — T037: use the rich telemetry snapshot
    if (display_state == sr::SessionState::Recording ||
        display_state == sr::SessionState::Paused ||
        display_state == sr::SessionState::Stopping) {
        auto ts = g_controller.telemetry_snapshot();
        wchar_t fps_buf[120];
        _snwprintf_s(fps_buf, _countof(fps_buf), _TRUNCATE,
            L"Cap:%u  Enc:%u  Drop:%u  Queue:%u  Enc:%s%s",
            ts.frames_captured, ts.frames_encoded, ts.frames_dropped,
            ts.frames_backlogged,
            ts.encoder_mode_label(),
            ts.is_on_ac ? L"" : L"  Battery");
        SetWindowTextW(g_lbl_fps, fps_buf);

        wchar_t drp_buf[64];
        _snwprintf_s(drp_buf, _countof(drp_buf), _TRUNCATE,
            L"Dup:%u  AudioPkts:%u",
            ts.dup_frames, ts.audio_packets);
        SetWindowTextW(g_lbl_dropped, drp_buf);
    } else {
        SetWindowTextW(g_lbl_fps,     L"Cap:0  Enc:0  Drop:0  Queue:0");
        SetWindowTextW(g_lbl_dropped, L"Dup:0  AudioPkts:0");
    }

    // Output path
    const std::wstring& out_path = g_controller.output_path();
    SetWindowTextW(g_lbl_path, out_path.empty()
        ? g_storage.outputDirectory().c_str()
        : out_path.c_str());

    const auto status_tone = StatusToneForState(display_state);
    const auto status = sr::ui::status_visual(status_tone, g_motion_enabled);
    if (state_changed || status.animated) {
        InvalidateStatusChrome(g_hwnd);
    }
}

static void ApplyUIFont(HWND hwnd) {
    if (!g_font_ui || !g_font_bold) return;
    HFONT metric_font = g_font_metric ? g_font_metric : g_font_ui;
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
    if (g_btn_start)    SendMessageW(g_btn_start,    WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    if (g_btn_stop)     SendMessageW(g_btn_stop,     WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    if (g_btn_pause)    SendMessageW(g_btn_pause,    WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    if (g_btn_mute)     SendMessageW(g_btn_mute,     WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    if (g_btn_settings) SendMessageW(g_btn_settings, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
    if (g_btn_hq)       SendMessageW(g_btn_hq,       WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
    if (g_lbl_status)   SendMessageW(g_lbl_status,   WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    if (g_lbl_time)     SendMessageW(g_lbl_time,     WM_SETFONT, reinterpret_cast<WPARAM>(metric_font), TRUE);
    if (g_lbl_fps)      SendMessageW(g_lbl_fps,      WM_SETFONT, reinterpret_cast<WPARAM>(metric_font), TRUE);
    if (g_lbl_path)     SendMessageW(g_lbl_path,     WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
    if (g_lbl_dropped)  SendMessageW(g_lbl_dropped,  WM_SETFONT, reinterpret_cast<WPARAM>(metric_font), TRUE);
    if (g_lbl_profile)  SendMessageW(g_lbl_profile,  WM_SETFONT, reinterpret_cast<WPARAM>(metric_font), TRUE);
}

static void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static bool IsControlEnabled(HWND hwnd, UINT id) {
    HWND ctrl = GetDlgItem(hwnd, static_cast<int>(id));
    return ctrl && IsWindowEnabled(ctrl);
}

static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd,
                                           UINT msg,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           UINT_PTR subclass_id,
                                           DWORD_PTR ref_data)
{
    (void)subclass_id;
    (void)ref_data;

    switch (msg) {
    case WM_MOUSEMOVE:
        if (g_hot_button != hwnd) {
            HWND old_hot = g_hot_button;
            g_hot_button = hwnd;
            if (old_hot) InvalidateRect(old_hot, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hot_button == hwnd) {
            g_hot_button = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        if (g_hot_button == hwnd) {
            g_hot_button = nullptr;
        }
        RemoveWindowSubclass(hwnd, ButtonSubclassProc, 1);
        break;
    default:
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void InstallButtonHover(HWND hwnd) {
    SetWindowSubclass(hwnd, ButtonSubclassProc, 1, 0);
}

static void DrawButton(HDC hdc, const RECT& rc, const std::wstring& text,
                       bool enabled, bool pressed, bool hot, bool focused, bool primary)
{
    const auto role = primary ? sr::ui::ButtonRole::Primary : sr::ui::ButtonRole::Secondary;
    const auto interaction =
        !enabled ? sr::ui::ButtonInteraction::Disabled :
        pressed  ? sr::ui::ButtonInteraction::Pressed :
        hot      ? sr::ui::ButtonInteraction::Hot :
                   sr::ui::ButtonInteraction::Normal;
    const auto visual = sr::ui::button_visual(role, interaction);

    FillRect(hdc, &rc, g_brush_bg);

    HBRUSH fill_brush = CreateSolidBrush(visual.fill);
    HPEN border_pen = CreatePen(PS_SOLID, 1, visual.border);
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, fill_brush);

    const int radius = sr::ui::kButtonCornerRadius * 2;
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(fill_brush);
    DeleteObject(border_pen);

    if (focused && enabled) {
        RECT focus_rc = rc;
        InflateRect(&focus_rc, -sr::ui::kButtonFocusInset, -sr::ui::kButtonFocusInset);
        HPEN focus_pen = CreatePen(PS_SOLID, 1, sr::ui::kFocusRing);
        old_pen = SelectObject(hdc, focus_pen);
        old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(hdc, focus_rc.left, focus_rc.top, focus_rc.right, focus_rc.bottom,
                  radius, radius);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(focus_pen);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, visual.text);

    RECT text_rc = rc;
    DrawTextW(hdc, text.c_str(), -1, &text_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void DrawStatusPill(HDC hdc, const RECT& client_rc, sr::SessionState state)
{
    const auto tone = StatusToneForState(state);
    const auto visual = sr::ui::status_visual(tone, g_motion_enabled);
    const bool pulse_on = ((GetTickCount64() / 500ULL) % 2ULL) == 0ULL;
    const COLORREF accent =
        sr::ui::status_pulse_color(tone, pulse_on, g_motion_enabled);

    RECT pill{client_rc.right - 142, 10, client_rc.right - 16, 34};
    if (pill.left < 300) {
        pill.left = 300;
    }

    HBRUSH fill_brush = CreateSolidBrush(visual.fill);
    HPEN border_pen = CreatePen(PS_SOLID, 1, visual.border);
    HGDIOBJ old_pen = SelectObject(hdc, border_pen);
    HGDIOBJ old_brush = SelectObject(hdc, fill_brush);
    RoundRect(hdc, pill.left, pill.top, pill.right, pill.bottom, 12, 12);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(fill_brush);
    DeleteObject(border_pen);

    RECT dot{pill.left + 12, pill.top + 8, pill.left + 20, pill.top + 16};
    HBRUSH dot_brush = CreateSolidBrush(accent);
    HPEN dot_pen = CreatePen(PS_SOLID, 1, accent);
    old_pen = SelectObject(hdc, dot_pen);
    old_brush = SelectObject(hdc, dot_brush);
    Ellipse(hdc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(dot_brush);
    DeleteObject(dot_pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, visual.text);
    HGDIOBJ old_font = nullptr;
    if (g_font_metric) {
        old_font = SelectObject(hdc, g_font_metric);
    }
    RECT text_rc{pill.left + 28, pill.top + 3, pill.right - 10, pill.bottom - 3};
    DrawTextW(hdc, StatusPillLabel(state), -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (old_font) {
        SelectObject(hdc, old_font);
    }
}

// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        g_motion_enabled = IsClientAreaAnimationEnabled();
        int y = 14;

        g_font_ui = CreateFontW(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_font_bold = CreateFontW(
            -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_font_title = CreateFontW(
            -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_font_metric = CreateFontW(
            -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");

        g_lbl_title = CreateWindowW(L"STATIC", L"Screen Recorder",
                                    WS_VISIBLE | WS_CHILD,
                                    12, y, 250, 24, hwnd, nullptr, nullptr, nullptr);
        if (g_font_title) {
            SendMessageW(g_lbl_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_title), TRUE);
        }

        y += 30;
        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD,
                      12, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_status = CreateWindowW(L"STATIC", L"Idle", WS_VISIBLE | WS_CHILD,
                       78, y, 230, 20, hwnd, (HMENU)ID_LABEL_STATUS, nullptr, nullptr);

        y += 30;
        CreateWindowW(L"STATIC", L"Time:", WS_VISIBLE | WS_CHILD,
                      12, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_time = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD,
                     78, y, 120, 20, hwnd, (HMENU)ID_LABEL_TIME, nullptr, nullptr);

        y += 28;
        g_lbl_fps = CreateWindowW(L"STATIC", L"Captured:0  Encoded:0",
                    WS_VISIBLE | WS_CHILD,
                    12, y, 700, 20, hwnd, (HMENU)ID_LABEL_FPS, nullptr, nullptr);

        y += 24;
        g_lbl_dropped = CreateWindowW(L"STATIC", L"Dropped: 0",
                        WS_VISIBLE | WS_CHILD,
                        12, y, 360, 20, hwnd, (HMENU)ID_LABEL_DROPPED, nullptr, nullptr);

        y += 30;
        g_lbl_path = CreateWindowW(L"STATIC", g_storage.outputDirectory().c_str(),
                     WS_VISIBLE | WS_CHILD | SS_PATHELLIPSIS,
                     12, y, 700, 20, hwnd, (HMENU)ID_LABEL_PATH, nullptr, nullptr);

        y += 38;
        g_btn_start = CreateWindowW(L"BUTTON", L"Start",
                      WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                      12,  y, 120, 32, hwnd, (HMENU)ID_BTN_START, nullptr, nullptr);
        g_btn_stop  = CreateWindowW(L"BUTTON", L"Stop",
                      WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                      144, y, 120, 32, hwnd, (HMENU)ID_BTN_STOP, nullptr, nullptr);
        g_btn_pause = CreateWindowW(L"BUTTON", L"Pause",
                      WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                      276, y, 120, 32, hwnd, (HMENU)ID_BTN_PAUSE, nullptr, nullptr);
        g_btn_mute  = CreateWindowW(L"BUTTON", L"Mute",
                      WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                      408, y, 120, 32, hwnd, (HMENU)ID_BTN_MUTE, nullptr, nullptr);

        y += 42;
        g_btn_settings = CreateWindowW(L"BUTTON", L"Settings",
                         WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                         12, y, 132, 28, hwnd, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
        g_btn_hq = CreateWindowW(L"BUTTON", L"HQ Off",
                   WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
                   156, y, 92, 28, hwnd, (HMENU)ID_BTN_HQ, nullptr, nullptr);

        InstallButtonHover(g_btn_start);
        InstallButtonHover(g_btn_stop);
        InstallButtonHover(g_btn_pause);
        InstallButtonHover(g_btn_mute);
        InstallButtonHover(g_btn_settings);
        InstallButtonHover(g_btn_hq);

        // Profile label: e.g. "30fps | 848x480 | 4Mbps"
        g_lbl_profile = CreateWindowW(L"STATIC", L"30fps | 848x480 | 4Mbps",
                        WS_VISIBLE | WS_CHILD | SS_LEFT,
                        262, y + 6, 450, 18, hwnd, (HMENU)ID_LABEL_PROFILE, nullptr, nullptr);

        ApplyUIFont(hwnd);
        LayoutMainWindow(hwnd);

        SetTimer(hwnd, ID_TIMER_UPDATE, 250, nullptr);
        UpdateUI();
        break;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!dis || dis->CtlType != ODT_BUTTON) break;

        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, _countof(text));

        const UINT id = static_cast<UINT>(dis->CtlID);
        const bool enabled = (dis->itemState & ODS_DISABLED) == 0;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool hot = ((dis->itemState & ODS_HOTLIGHT) != 0) || (dis->hwndItem == g_hot_button);
        const bool focused = (dis->itemState & ODS_FOCUS) != 0;
        const bool primary = (id == ID_BTN_START);

        DrawButton(dis->hDC, dis->rcItem, text, enabled, pressed, hot, focused, primary);
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brush_bg);

        RECT header{0, 0, rc.right, 44};
        FillRect(hdc, &header, g_brush_header);

        RECT accent{0, 0, 3, 44};
        const auto state = g_controller.state();
        const auto tone = StatusToneForState(state);
        const bool pulse_on = ((GetTickCount64() / 500ULL) % 2ULL) == 0ULL;
        HBRUSH accent_brush = CreateSolidBrush(
            sr::ui::status_pulse_color(tone, pulse_on, g_motion_enabled));
        FillRect(hdc, &accent, accent_brush);
        DeleteObject(accent_brush);

        DrawStatusPill(hdc, rc, state);

        HPEN pen = CreatePen(PS_SOLID, 1, kBorderColor);
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        MoveToEx(hdc, 0, 44, nullptr);
        LineTo(hdc, rc.right, 44);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);
        if (child == g_lbl_profile || child == g_lbl_path) {
            SetTextColor(hdc, kMutedText);
        } else if (child == g_lbl_title) {
            SetTextColor(hdc, sr::ui::kTextStrong);
            return reinterpret_cast<INT_PTR>(g_brush_header);
        } else {
            SetTextColor(hdc, kTextColor);
        }
        return reinterpret_cast<INT_PTR>(g_brush_bg);
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, kBgColor);
        SetTextColor(hdc, kTextColor);
        return reinterpret_cast<INT_PTR>(g_brush_bg);
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_UPDATE) {
            UpdateUI();
            g_camera_overlay.refresh_power_profile();
        }
        break;

    case WM_SETTINGCHANGE:
        g_motion_enabled = IsClientAreaAnimationEnabled();
        InvalidateStatusChrome(hwnd);
        break;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            mmi->ptMinTrackSize.x = sr::ui::kMainWindowMinWidth;
            mmi->ptMinTrackSize.y = sr::ui::kMainWindowMinHeight;
        }
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            LayoutMainWindow(hwnd);
        }
        return 0;

    case WM_SR_STATUS: {
        auto* s = reinterpret_cast<wchar_t*>(lParam);
        if (s) { SetWindowTextW(g_lbl_status, s); delete[] s; }
        break;
    }
    case WM_SR_ERROR: {
        auto* s = reinterpret_cast<wchar_t*>(lParam);
        if (s) {
            MessageBoxW(hwnd, s, L"Screen Recorder Error", MB_ICONERROR | MB_OK);
            delete[] s;
        }
        UpdateUI();
        break;
    }
    case WM_SR_STOP_COMPLETE: {
        JoinStopThreadIfFinished();
        const bool exit_after_stop = g_stop_flow.complete_stop();
        if (wParam != 0) {
            SR_LOG_INFO(L"Recording stopped");
        }
        UpdateUI();
        if (exit_after_stop) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case ID_BTN_START:
            if (g_controller.start()) {
                g_record_start_ms = static_cast<int64_t>(sr::QPCClock::instance().now_ms());
                g_paused_total_ms = 0;
                SR_LOG_INFO(L"Recording started");
            }
            UpdateUI();
            break;

        case ID_BTN_STOP:
            BeginStopAsync(hwnd, false);
            break;

        case ID_BTN_PAUSE:
            if (g_controller.is_recording()) {
                if (g_controller.pause()) {
                    g_pause_start_ms = static_cast<int64_t>(sr::QPCClock::instance().now_ms());
                    SR_LOG_INFO(L"Recording paused");
                }
            } else if (g_controller.is_paused()) {
                if (g_controller.resume()) {
                    g_paused_total_ms += static_cast<int64_t>(
                        sr::QPCClock::instance().now_ms()) - g_pause_start_ms;
                    SR_LOG_INFO(L"Recording resumed");
                }
            }
            UpdateUI();
            break;

        case ID_BTN_MUTE:
            g_controller.set_muted(!g_controller.is_muted());
            SR_LOG_INFO(L"Mic %s", g_controller.is_muted() ? L"muted" : L"unmuted");
            UpdateUI();
            break;

        case ID_BTN_SETTINGS:
            if (!g_controller.state_is_idle()) {
                MessageBoxW(hwnd,
                    L"Please stop the recording before changing settings.",
                    L"Settings", MB_ICONINFORMATION | MB_OK);
                break;
            }
            if (sr::ShowSettingsDialog(hwnd, g_settings)) {
                const bool camera_was_running = g_camera_overlay.is_running();
                if (camera_was_running) {
                    g_camera_overlay.stop();
                }

                // Apply output directory
                if (!g_settings.output_dir.empty()) {
                    g_storage.setOutputDirectory(g_settings.output_dir);
                } else {
                    g_storage.resolveDefaultDirectory();
                }
                ApplyEncoderProfileFromSettings();
                ApplyCameraProfileFromSettings();
                // Persist
                g_settings.save();

                // Camera overlay
                if (g_settings.camera_overlay_enabled) {
                    g_camera_overlay.start(g_hwnd);
                }

                UpdateProfileLabel();
                // Refresh path display
                SetWindowTextW(g_lbl_path, g_storage.outputDirectory().c_str());
                SR_LOG_INFO(L"Settings applied: %u fps, high_quality=%s, dir=%s",
                            g_settings.fps,
                            g_settings.high_quality ? L"on" : L"off",
                            g_settings.output_dir.empty() ? L"(default)" : g_settings.output_dir.c_str());
            }
            break;

        case ID_BTN_HQ:
            if (!g_controller.state_is_idle()) {
                MessageBoxW(hwnd,
                    L"Please stop the recording before changing High Quality mode.",
                    L"High Quality", MB_ICONINFORMATION | MB_OK);
                break;
            }
            g_settings.set_high_quality(!g_settings.high_quality);
            ApplyEncoderProfileFromSettings();
            if (g_settings.camera_overlay_enabled && g_camera_overlay.is_running()) {
                g_camera_overlay.stop();
                ApplyCameraProfileFromSettings();
                g_camera_overlay.start(g_hwnd);
            } else {
                ApplyCameraProfileFromSettings();
            }
            g_settings.save();
            UpdateProfileLabel();
            SR_LOG_INFO(L"High Quality toggled: %s (%u bps)",
                        g_settings.high_quality ? L"on" : L"off",
                        g_settings.bitrate_bps);
            UpdateUI();
            break;
        }
        break;

    case WM_CLOSE:
        if (!g_controller.state_is_idle() || g_stop_flow.stop_in_progress()) {
            BeginStopAsync(hwnd, true);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_UPDATE);
        JoinStopThreadIfFinished();
        if (!g_controller.state_is_idle()) g_controller.stop();
        g_camera_overlay.stop();
        if (g_font_ui) {
            DeleteObject(g_font_ui);
            g_font_ui = nullptr;
        }
        if (g_font_bold) {
            DeleteObject(g_font_bold);
            g_font_bold = nullptr;
        }
        if (g_font_title) {
            DeleteObject(g_font_title);
            g_font_title = nullptr;
        }
        if (g_font_metric) {
            DeleteObject(g_font_metric);
            g_font_metric = nullptr;
        }
        if (g_brush_header) {
            DeleteObject(g_brush_header);
            g_brush_header = nullptr;
        }
        if (g_brush_bg) {
            DeleteObject(g_brush_bg);
            g_brush_bg = nullptr;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!g_brush_bg) {
        g_brush_bg = CreateSolidBrush(kBgColor);
    }
    if (!g_brush_header) {
        g_brush_header = CreateSolidBrush(sr::ui::kHeaderBackground);
    }

#ifdef _DEBUG
    // Attach a console in debug builds so SR_LOG_* output (stderr) is visible
    if (AllocConsole()) {
        FILE* fp = nullptr;
        _wfreopen_s(&fp, L"CONOUT$", L"w", stderr);
        _wfreopen_s(&fp, L"CONOUT$", L"w", stdout);
    }
#endif

    // T033: Process priority is managed dynamically:
    // - NORMAL_PRIORITY_CLASS when idle (saves battery)
    // - ABOVE_NORMAL_PRIORITY_CLASS during recording (set by SessionController::start())
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    SR_LOG_INFO(L"ScreenRecorder starting...");

    // Load persisted settings and apply to storage + encoder profile
    g_settings.load();
    if (!g_settings.output_dir.empty()) {
        g_storage.setOutputDirectory(g_settings.output_dir);
    }
    {
        ApplyEncoderProfileFromSettings();
        ApplyCameraProfileFromSettings();
    }

    g_controller.initialize(
        &g_storage,
        [](const std::wstring& status) {
            if (!g_hwnd) return;
            auto* copy = new wchar_t[status.size() + 1];
            wcscpy_s(copy, status.size() + 1, status.c_str());
            PostMessageW(g_hwnd, WM_SR_STATUS, 0, reinterpret_cast<LPARAM>(copy));
        },
        [](const std::wstring& error) {
            if (!g_hwnd) return;
            auto* copy = new wchar_t[error.size() + 1];
            wcscpy_s(copy, error.size() + 1, error.c_str());
            PostMessageW(g_hwnd, WM_SR_ERROR, 0, reinterpret_cast<LPARAM>(copy));
        }
    );

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brush_bg;
    wc.lpszClassName = L"ScreenRecorderClass";
    wc.hIcon         = sr::load_app_icon(hInstance);
    wc.hIconSm       = sr::load_app_icon(hInstance,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"ScreenRecorderClass", sr::kAppWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 430,
        nullptr, nullptr, hInstance, nullptr
    );

    EnableDarkTitleBar(g_hwnd);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // T043: Check WGC availability — disable Start if screen capture is unsupported.
    if (!sr::CaptureEngine::is_wgc_supported()) {
        SR_LOG_ERROR(L"[T043] Windows Graphics Capture is not supported on this system.");
        MessageBoxW(g_hwnd,
            L"Screen capture is not available on this system.\n"
            L"Requires Windows 10 version 1903 (build 18362) or later.\n\n"
            L"Recording is disabled.",
            L"Screen Capture Unavailable", MB_ICONERROR | MB_OK);
        EnableWindow(g_btn_start, FALSE);
    }

    // T030: Orphan detection — scan for *.partial.mp4 left by a previous crash
    {
        auto orphans = g_storage.findOrphanedFiles();
        for (const auto& orphan : orphans) {
            std::wstring msg =
                L"An incomplete recording was found:\n\n" + orphan +
                L"\n\nWhat would you like to do?\n\n"
                L"Yes     \u2192 Recover (rename to .mp4 for playback)\n"
                L"No      \u2192 Delete the incomplete file\n"
                L"Cancel  \u2192 Ignore (keep as-is)";
            int choice = MessageBoxW(g_hwnd, msg.c_str(),
                                     L"Incomplete Recording Found",
                                     MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDYES) {
                std::wstring final_path = sr::StorageManager::partialToFinal(orphan);
                if (MoveFileExW(orphan.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    SR_LOG_INFO(L"Orphan recovered: %s", final_path.c_str());
                    MessageBoxW(g_hwnd,
                        (L"Recording recovered:\n" + final_path).c_str(),
                        L"Recovery Complete", MB_ICONINFORMATION | MB_OK);
                } else {
                    SR_LOG_ERROR(L"Orphan recovery failed: %u", GetLastError());
                }
            } else if (choice == IDNO) {
                DeleteFileW(orphan.c_str());
                SR_LOG_INFO(L"Orphan deleted: %s", orphan.c_str());
            }
            // IDCANCEL = ignore, leave file as-is
        }
    }

    UpdateProfileLabel();

    if (g_settings.camera_overlay_enabled) {
        g_camera_overlay.start(g_hwnd);
    }

    MSG msg_loop{};
    while (GetMessageW(&msg_loop, nullptr, 0, 0)) {
        TranslateMessage(&msg_loop);
        DispatchMessageW(&msg_loop);
    }

    CoUninitialize();
    if (g_brush_bg) {
        DeleteObject(g_brush_bg);
        g_brush_bg = nullptr;
    }
    return static_cast<int>(msg_loop.wParam);
}

