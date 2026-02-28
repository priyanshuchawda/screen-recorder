// main.cpp — ScreenRecorder entry point
// Minimal Win32 window with Start/Stop/Pause/Mute controls

#include <windows.h>
#include <commctrl.h>
#include <string>
#include "utils/logging.h"
#include "utils/qpc_clock.h"
#include "controller/session_machine.h"
#include "storage/storage_manager.h"

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
#define ID_TIMER_UPDATE  1

// Global state
static sr::SessionMachine g_machine;
static sr::StorageManager g_storage;
static HWND g_hwnd = nullptr;
static HWND g_btn_start = nullptr;
static HWND g_btn_stop = nullptr;
static HWND g_btn_pause = nullptr;
static HWND g_btn_mute = nullptr;
static HWND g_lbl_status = nullptr;
static HWND g_lbl_time = nullptr;
static HWND g_lbl_fps = nullptr;
static HWND g_lbl_path = nullptr;
static int64_t g_record_start_ms = 0;
static int64_t g_paused_total_ms = 0;
static int64_t g_pause_start_ms = 0;
static bool g_muted = false;

void UpdateUI() {
    auto state = g_machine.state();
    const wchar_t* status_text = sr::state_name(state);

    SetWindowTextW(g_lbl_status, status_text);

    EnableWindow(g_btn_start, state == sr::SessionState::Idle);
    EnableWindow(g_btn_stop, state == sr::SessionState::Recording || state == sr::SessionState::Paused);
    EnableWindow(g_btn_pause, state == sr::SessionState::Recording || state == sr::SessionState::Paused);
    EnableWindow(g_btn_mute, state == sr::SessionState::Recording || state == sr::SessionState::Paused);

    if (state == sr::SessionState::Paused) {
        SetWindowTextW(g_btn_pause, L"Resume");
    } else {
        SetWindowTextW(g_btn_pause, L"Pause");
    }

    SetWindowTextW(g_btn_mute, g_muted ? L"Unmute" : L"Mute");

    // Update elapsed time
    if (state == sr::SessionState::Recording || state == sr::SessionState::Paused) {
        double now_ms = sr::QPCClock::instance().now_ms();
        int64_t elapsed_ms = static_cast<int64_t>(now_ms) - g_record_start_ms - g_paused_total_ms;
        if (state == sr::SessionState::Paused) {
            elapsed_ms = g_pause_start_ms - g_record_start_ms - g_paused_total_ms;
        }
        int secs = static_cast<int>(elapsed_ms / 1000);
        int mins = secs / 60;
        secs %= 60;
        int hrs = mins / 60;
        mins %= 60;

        wchar_t buf[64];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%02d:%02d:%02d", hrs, mins, secs);
        SetWindowTextW(g_lbl_time, buf);
    } else {
        SetWindowTextW(g_lbl_time, L"00:00:00");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Status bar area
        int y = 10;
        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD, 10, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_status = CreateWindowW(L"STATIC", L"Idle", WS_VISIBLE | WS_CHILD, 75, y, 120, 20, hwnd, (HMENU)ID_LABEL_STATUS, nullptr, nullptr);

        y += 30;
        CreateWindowW(L"STATIC", L"Time:", WS_VISIBLE | WS_CHILD, 10, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_time = CreateWindowW(L"STATIC", L"00:00:00", WS_VISIBLE | WS_CHILD, 75, y, 120, 20, hwnd, (HMENU)ID_LABEL_TIME, nullptr, nullptr);

        y += 30;
        CreateWindowW(L"STATIC", L"FPS:", WS_VISIBLE | WS_CHILD, 10, y, 60, 20, hwnd, nullptr, nullptr, nullptr);
        g_lbl_fps = CreateWindowW(L"STATIC", L"0", WS_VISIBLE | WS_CHILD, 75, y, 120, 20, hwnd, (HMENU)ID_LABEL_FPS, nullptr, nullptr);

        y += 30;
        g_lbl_path = CreateWindowW(L"STATIC", g_storage.outputDirectory().c_str(),
                                   WS_VISIBLE | WS_CHILD | SS_PATHELLIPSIS, 10, y, 340, 20, hwnd, (HMENU)ID_LABEL_PATH, nullptr, nullptr);

        // Buttons
        y += 40;
        g_btn_start = CreateWindowW(L"BUTTON", L"Start", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                     10, y, 80, 30, hwnd, (HMENU)ID_BTN_START, nullptr, nullptr);
        g_btn_stop = CreateWindowW(L"BUTTON", L"Stop", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                    100, y, 80, 30, hwnd, (HMENU)ID_BTN_STOP, nullptr, nullptr);
        g_btn_pause = CreateWindowW(L"BUTTON", L"Pause", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                     190, y, 80, 30, hwnd, (HMENU)ID_BTN_PAUSE, nullptr, nullptr);
        g_btn_mute = CreateWindowW(L"BUTTON", L"Mute", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                    280, y, 80, 30, hwnd, (HMENU)ID_BTN_MUTE, nullptr, nullptr);

        SetTimer(hwnd, ID_TIMER_UPDATE, 200, nullptr);
        UpdateUI();
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_UPDATE) {
            UpdateUI();
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_START:
            if (g_machine.transition(sr::SessionEvent::Start)) {
                g_record_start_ms = static_cast<int64_t>(sr::QPCClock::instance().now_ms());
                g_paused_total_ms = 0;
                g_muted = false;
                SR_LOG_INFO(L"Recording started");
            }
            UpdateUI();
            break;

        case ID_BTN_STOP:
            if (g_machine.transition(sr::SessionEvent::Stop)) {
                if (g_machine.state() == sr::SessionState::Stopping) {
                    // Simulate finalization completing instantly for now
                    g_machine.transition(sr::SessionEvent::Finalized);
                }
                SR_LOG_INFO(L"Recording stopped");
            }
            UpdateUI();
            break;

        case ID_BTN_PAUSE:
            if (g_machine.is_recording()) {
                if (g_machine.transition(sr::SessionEvent::Pause)) {
                    g_pause_start_ms = static_cast<int64_t>(sr::QPCClock::instance().now_ms());
                    SR_LOG_INFO(L"Recording paused");
                }
            } else if (g_machine.is_paused()) {
                if (g_machine.transition(sr::SessionEvent::Resume)) {
                    g_paused_total_ms += static_cast<int64_t>(sr::QPCClock::instance().now_ms()) - g_pause_start_ms;
                    SR_LOG_INFO(L"Recording resumed");
                }
            }
            UpdateUI();
            break;

        case ID_BTN_MUTE:
            g_muted = !g_muted;
            SR_LOG_INFO(L"Mic %s", g_muted ? L"muted" : L"unmuted");
            UpdateUI();
            break;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_UPDATE);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // Initialize COM for Media Foundation
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    SR_LOG_INFO(L"ScreenRecorder starting...");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ScreenRecorderClass";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"ScreenRecorderClass", L"Screen Recorder",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 230,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
