// Local, opt-in end-to-end probe. Writes only to the directory supplied by the
// caller. The animated window makes capture progression visible in the MP4.
#include <windows.h>
#include <mfapi.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include "controller/session_controller.h"
#include "storage/storage_manager.h"
#include "storage/probe_fault_injection.h"

static LRESULT CALLBACK probe_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_TIMER:
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        wchar_t text[128]{};
        swprintf_s(text, L"Screen recorder local diagnostic: %llu ms", GetTickCount64());
        TextOutW(dc, 20, 20, text, static_cast<int>(wcslen(text)));
        const int x = 20 + static_cast<int>((GetTickCount64() / 10) % 400);
        RECT marker{x, 70, x + 60, 130};
        FillRect(dc, &marker, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5) {
        std::fwprintf(stderr, L"Usage: recording_probe OUTPUT_DIRECTORY SECONDS [15] [--pause-resume|--stop-paused]\n");
        return 2;
    }
    const int seconds = _wtoi(argv[2]);
    if (seconds < 1 || seconds > 3600) return 2;
    bool low_rate = false;
    bool pause_resume_test = false;
    bool stop_paused_test = false;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"15") == 0 && !low_rate) {
            low_rate = true;
        } else if (wcscmp(argv[i], L"--pause-resume") == 0 && !pause_resume_test) {
            pause_resume_test = true;
        } else if (wcscmp(argv[i], L"--stop-paused") == 0 && !stop_paused_test) {
            stop_paused_test = true;
        } else {
            std::fwprintf(stderr, L"Invalid or repeated probe option: %ls\n", argv[i]);
            return 2;
        }
    }
    if ((pause_resume_test && stop_paused_test) ||
        ((pause_resume_test || stop_paused_test) && seconds < 6)) return 2;
#ifdef SR_LOCAL_RECORDING_PROBE
    wchar_t stall_value[2]{};
    const bool stall_requested =
        GetEnvironmentVariableW(L"SR_PROBE_STALL_AUDIO", stall_value, 2) == 1 &&
        stall_value[0] == L'1';
#else
    const bool stall_requested = false;
#endif
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    WNDCLASSW cls{};
    cls.lpfnWndProc = probe_window;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"ScreenRecorderLocalProbe";
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls)) return 4;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  cls.lpszClassName, L"Screen recorder local diagnostic",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 640, 240,
                                  nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 5;
    SetTimer(window, 1, 16, nullptr);
    std::atomic<bool> stop_requested{false};
    int result = 0;
    {
        sr::StorageManager storage;
        sr::SessionController controller;
        if (!storage.setOutputDirectory(argv[1]) ||
            !controller.initialize(&storage, nullptr,
                [](const std::wstring& error) { std::fwprintf(stderr, L"Error: %ls\n", error.c_str()); },
                [&stop_requested]() { stop_requested.store(true); })) return 6;
        sr::EncoderProfile profile{};
        profile.width = 1920; profile.height = 1080;
        profile.fps = 60; profile.bitrate_bps = 10'000'000;
        if (low_rate) {
            profile.width = 640; profile.height = 360;
            profile.fps = 15; profile.bitrate_bps = 1'000'000;
        }
        controller.set_encoder_profile(profile, !low_rate);
        if (!controller.start()) return 7;
        std::fwprintf(stdout, L"Recording: %ls\n", controller.output_path().c_str());
        std::fflush(stdout);
        const ULONGLONG test_started = GetTickCount64();
        const ULONGLONG deadline = test_started + static_cast<ULONGLONG>(seconds) * 1000;
        ULONGLONG pause_started = 0;
        bool did_pause = false;
        bool did_resume = false;
        bool resume_attempted = false;
        bool stop_while_paused = false;
        bool quit = false;
        while (!quit && !stop_requested.load() && GetTickCount64() < deadline) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsed = now - test_started;
            if (pause_resume_test && !did_pause && elapsed >=
                static_cast<ULONGLONG>(seconds) * 1000 / 3) {
                did_pause = controller.pause();
                pause_started = now;
                std::fwprintf(stdout, L"Pause test: paused=%u\n", did_pause ? 1u : 0u);
            }
            if (pause_resume_test && did_pause && !resume_attempted && now - pause_started >= 3000) {
                resume_attempted = true;
                did_resume = controller.resume();
                std::fwprintf(stdout, L"Pause test: resumed=%u\n", did_resume ? 1u : 0u);
            }
            if (stop_paused_test && elapsed >=
                static_cast<ULONGLONG>(seconds) * 2 * 1000 / 3) {
                stop_while_paused = controller.pause();
                std::fwprintf(stdout, L"Stop-paused test: paused=%u; requesting stop\n",
                              stop_while_paused ? 1u : 0u);
                break;
            }
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        if ((pause_resume_test && (!did_pause || !did_resume)) ||
            (stop_paused_test && !stop_while_paused)) {
            std::fwprintf(stderr, L"FAIL: requested pause scenario did not complete.\n");
            result = 11;
        }
        if (!controller.stop()) result = 8;
        const auto outcome = controller.last_recording_outcome();
        const auto expected = stall_requested
            ? sr::RecordingOutcome::Incomplete
            : sr::RecordingOutcome::Complete;
        if (outcome != expected) {
            std::fwprintf(stderr, L"FAIL: expected recording outcome %ls, got %ls.\n",
                          sr::recording_outcome_name(expected),
                          sr::recording_outcome_name(outcome));
            result = 10;
        }
        if (stall_requested && !sr::probe_audio_stall_was_triggered()) {
            std::fwprintf(stderr, L"FAIL: requested audio writer stall was not triggered.\n");
            result = 9;
        }
        std::fwprintf(stdout, L"Stopped; outcome=%ls captured=%u encoded=%u dropped=%u audio_packets=%u\n",
                      sr::recording_outcome_name(outcome),
                      controller.frames_captured(), controller.frames_encoded(),
                      controller.frames_dropped(), controller.audio_packets_written());
    }
    DestroyWindow(window);
    MFShutdown();
    CoUninitialize();
    return result;
}
