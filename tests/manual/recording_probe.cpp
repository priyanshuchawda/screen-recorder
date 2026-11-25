// Local, opt-in end-to-end probe. Writes only to the directory supplied by the
// caller. The animated window makes capture progression visible in the MP4.
#include <windows.h>
#include <mfapi.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include "controller/session_controller.h"
#include "storage/storage_manager.h"

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
    if (argc != 3 && argc != 4) {
        std::fwprintf(stderr, L"Usage: recording_probe OUTPUT_DIRECTORY SECONDS [15]\n");
        return 2;
    }
    const int seconds = _wtoi(argv[2]);
    if (seconds < 1 || seconds > 3600) return 2;
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
        const bool low_rate = argc == 4 && _wtoi(argv[3]) == 15;
        if (argc == 4 && !low_rate) return 2;
        if (low_rate) {
            profile.width = 640; profile.height = 360;
            profile.fps = 15; profile.bitrate_bps = 1'000'000;
        }
        controller.set_encoder_profile(profile, !low_rate);
        if (!controller.start()) return 7;
        std::fwprintf(stdout, L"Recording: %ls\n", controller.output_path().c_str());
        std::fflush(stdout);
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
        bool quit = false;
        while (!quit && !stop_requested.load() && GetTickCount64() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        if (!controller.stop()) result = 8;
        std::fwprintf(stdout, L"Stopped; captured=%u encoded=%u dropped=%u audio_packets=%u\n",
                      controller.frames_captured(), controller.frames_encoded(),
                      controller.frames_dropped(), controller.audio_packets_written());
    }
    DestroyWindow(window);
    MFShutdown();
    CoUninitialize();
    return result;
}
