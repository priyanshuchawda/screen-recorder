// main.cpp — ScreenRecorder entry point
// T017: Minimal Win32 window wired to SessionController for Start/Stop/Pause/Mute
// T037: Debug telemetry overlay uses TelemetrySnapshot
// T043: WGC availability check disables Start button on unsupported hardware

#include <windows.h>
#include <processthreadsapi.h>
#include <commctrl.h>
#include <string>
#include <cstdio>
#include "utils/logging.h"
#include "utils/qpc_clock.h"
#include "storage/storage_manager.h"
#include "controller/session_controller.h"
#include "capture/capture_engine.h"   // T043: is_wgc_supported()
#include "app/app_settings.h"
#include "app/settings_dialog.h"
#include "app/telemetry.h"             // T037: TelemetrySnapshot

#pragma comment(lib, "comctl32.lib")

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
#define ID_TIMER_UPDATE  1

// Custom messages for marshalling background thread callbacks to UI thread
#define WM_SR_STATUS  (WM_USER + 1)
#define WM_SR_ERROR   (WM_USER + 2)

// Global state
static sr::AppSettings       g_settings;
static sr::StorageManager    g_storage;
static sr::SessionController g_controller;

static HWND g_hwnd          = nullptr;
static HWND g_btn_start     = nullptr;
static HWND g_btn_stop      = nullptr;
static HWND g_btn_pause     = nullptr;
static HWND g_btn_mute      = nullptr;
static HWND g_btn_settings  = nullptr;
static HWND g_lbl_status    = nullptr;
static HWND g_lbl_time      = nullptr;
static HWND g_lbl_fps       = nullptr;
static HWND g_lbl_path      = nullptr;
static HWND g_lbl_dropped   = nullptr;
static HWND g_lbl_profile   = nullptr;

static int64_t g_record_start_ms = 0;
static int64_t g_paused_total_ms = 0;
static int64_t g_pause_start_ms  = 0;

// ----------------------------------------------------------------------------
void UpdateUI()
{
    auto state = g_controller.state();

    switch (state) {
        case sr::SessionState::Idle:      SetWindowTextW(g_lbl_status, L"Idle");      break;
        case sr::SessionState::Recording: SetWindowTextW(g_lbl_status, L"Recording"); break;
        case sr::SessionState::Paused:    SetWindowTextW(g_lbl_status, L"Paused");    break;
        case sr::SessionState::Stopping:  SetWindowTextW(g_lbl_status, L"Stopping");  break;
    }

    bool can_start = (state == sr::SessionState::Idle);
    bool can_stop  = (state == sr::SessionState::Recording || state == sr::SessionState::Paused);

    EnableWindow(g_btn_start, can_start ? TRUE : FALSE);
    EnableWindow(g_btn_stop,  can_stop  ? TRUE : FALSE);
    EnableWindow(g_btn_pause, can_stop  ? TRUE : FALSE);
    EnableWindow(g_btn_mute,  can_stop  ? TRUE : FALSE);

    SetWindowTextW(g_btn_pause, (state == sr::SessionState::Paused) ? L"Resume" : L"Pause");
    SetWindowTextW(g_btn_mute,  g_controller.is_muted() ? L"Unmute" : L"Mute");

    // Elapsed time
    if (state == sr::SessionState::Recording || state == sr::SessionState::Paused) {
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
    if (state == sr::SessionState::Recording || state == sr::SessionState::Paused) {
        auto ts = g_controller.telemetry_snapshot();
        wchar_t fps_buf[120];
        _snwprintf_s(fps_buf, _countof(fps_buf), _TRUNCATE,
            L"Cap:%u  Enc:%u  Drop:%u  Queue:%u  Enc:%s%s",
            ts.frames_captured, ts.frames_encoded, ts.frames_dropped,
            ts.frames_backlogged,
            ts.encoder_mode_label(),
            ts.is_on_ac ? L"" : L" \U0001F50B");
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
}

// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        int y = 10;
        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD,
                      10, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_status = CreateWindowW(L"STATIC", L"Idle", WS_VISIBLE | WS_CHILD,
                       75, y, 210, 20, hwnd, (HMENU)ID_LABEL_STATUS, nullptr, nullptr);

        y += 26;
        CreateWindowW(L"STATIC", L"Time:", WS_VISIBLE | WS_CHILD,
                      10, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_time = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD,
                     75, y, 120, 20, hwnd, (HMENU)ID_LABEL_TIME, nullptr, nullptr);

        y += 24;
        g_lbl_fps = CreateWindowW(L"STATIC", L"Captured:0  Encoded:0",
                    WS_VISIBLE | WS_CHILD,
                    10, y, 350, 20, hwnd, (HMENU)ID_LABEL_FPS, nullptr, nullptr);

        y += 22;
        g_lbl_dropped = CreateWindowW(L"STATIC", L"Dropped: 0",
                        WS_VISIBLE | WS_CHILD,
                        10, y, 200, 20, hwnd, (HMENU)ID_LABEL_DROPPED, nullptr, nullptr);

        y += 26;
        g_lbl_path = CreateWindowW(L"STATIC", g_storage.outputDirectory().c_str(),
                     WS_VISIBLE | WS_CHILD | SS_PATHELLIPSIS,
                     10, y, 350, 20, hwnd, (HMENU)ID_LABEL_PATH, nullptr, nullptr);

        y += 36;
        g_btn_start = CreateWindowW(L"BUTTON", L"Start",
                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      10,  y, 80, 30, hwnd, (HMENU)ID_BTN_START, nullptr, nullptr);
        g_btn_stop  = CreateWindowW(L"BUTTON", L"Stop",
                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      100, y, 80, 30, hwnd, (HMENU)ID_BTN_STOP, nullptr, nullptr);
        g_btn_pause = CreateWindowW(L"BUTTON", L"Pause",
                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      190, y, 80, 30, hwnd, (HMENU)ID_BTN_PAUSE, nullptr, nullptr);
        g_btn_mute  = CreateWindowW(L"BUTTON", L"Mute",
                      WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      280, y, 80, 30, hwnd, (HMENU)ID_BTN_MUTE, nullptr, nullptr);

        y += 36;
        g_btn_settings = CreateWindowW(L"BUTTON", L"\u2699 Settings",
                         WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                         10, y, 100, 26, hwnd, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);

        // Profile label: e.g. "30 fps | 8 Mbps"
        g_lbl_profile = CreateWindowW(L"STATIC", L"30 fps | 8 Mbps",
                        WS_VISIBLE | WS_CHILD | SS_LEFT,
                        120, y + 5, 250, 18, hwnd, (HMENU)ID_LABEL_PROFILE, nullptr, nullptr);

        SetTimer(hwnd, ID_TIMER_UPDATE, 250, nullptr);
        UpdateUI();
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_UPDATE) UpdateUI();
        break;

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
            if (g_controller.stop()) {
                SR_LOG_INFO(L"Recording stopped");
            }
            UpdateUI();
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
                // Apply output directory
                if (!g_settings.output_dir.empty()) {
                    g_storage.setOutputDirectory(g_settings.output_dir);
                } else {
                    g_storage.resolveDefaultDirectory();
                }
                // Apply encoder profile for next recording
                sr::EncoderProfile profile;
                profile.fps         = g_settings.fps;
                profile.bitrate_bps = g_settings.bitrate_bps;
                g_controller.set_encoder_profile(profile);
                // Persist
                g_settings.save();
                // Update profile label
                wchar_t prof_buf[64];
                _snwprintf_s(prof_buf, _countof(prof_buf), _TRUNCATE,
                    L"%u fps  |  %u Mbps",
                    g_settings.fps, g_settings.bitrate_bps / 1'000'000);
                SetWindowTextW(g_lbl_profile, prof_buf);
                // Refresh path display
                SetWindowTextW(g_lbl_path, g_storage.outputDirectory().c_str());
                SR_LOG_INFO(L"Settings applied: %u fps, dir=%s",
                            g_settings.fps,
                            g_settings.output_dir.empty() ? L"(default)" : g_settings.output_dir.c_str());
            }
            break;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_UPDATE);
        if (!g_controller.state_is_idle()) g_controller.stop();
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

#ifdef _DEBUG
    // Attach a console in debug builds so SR_LOG_* output (stderr) is visible
    if (AllocConsole()) {
        FILE* fp = nullptr;
        _wfreopen_s(&fp, L"CONOUT$", L"w", stderr);
        _wfreopen_s(&fp, L"CONOUT$", L"w", stdout);
    }
#endif

    // T033: Elevate process to ABOVE_NORMAL so OS scheduler favours our
    // capture, encode and audio threads over typical background tasks.
    // This is safe for a single-purpose recorder app.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    SR_LOG_INFO(L"ScreenRecorder starting...");

    // Load persisted settings and apply to storage + encoder profile
    g_settings.load();
    if (!g_settings.output_dir.empty()) {
        g_storage.setOutputDirectory(g_settings.output_dir);
    }
    {
        sr::EncoderProfile profile;
        profile.fps         = g_settings.fps;
        profile.bitrate_bps = g_settings.bitrate_bps;
        g_controller.set_encoder_profile(profile);
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ScreenRecorderClass";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"ScreenRecorderClass", L"Screen Recorder v1.0",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 390, 310,
        nullptr, nullptr, hInstance, nullptr
    );

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

    // Reflect loaded settings in the profile label
    if (g_lbl_profile) {
        wchar_t prof_buf[64];
        _snwprintf_s(prof_buf, _countof(prof_buf), _TRUNCATE,
            L"%u fps  |  %u Mbps",
            g_settings.fps, g_settings.bitrate_bps / 1'000'000);
        SetWindowTextW(g_lbl_profile, prof_buf);
    }

    MSG msg_loop{};
    while (GetMessageW(&msg_loop, nullptr, 0, 0)) {
        TranslateMessage(&msg_loop);
        DispatchMessageW(&msg_loop);
    }

    CoUninitialize();
    return static_cast<int>(msg_loop.wParam);
}

