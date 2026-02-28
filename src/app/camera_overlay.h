#pragma once

#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

namespace sr {

class CameraOverlay {
public:
    CameraOverlay() = default;
    ~CameraOverlay() { stop(); }

    bool start(HWND owner);
    void stop();
    void refresh_power_profile();

    bool is_running() const { return running_; }

private:
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void capture_loop();
    void draw_latest_frame(HDC hdc, const RECT& rc);
    void resize_capture_to_client();

    bool detect_on_battery() const;
    void apply_preview_tuning();

    HWND owner_ = nullptr;
    HWND host_hwnd_ = nullptr;

    std::thread capture_thread_;
    std::atomic<bool> capture_running_{ false };
    std::atomic<int>  capture_interval_ms_{ 33 };

    std::mutex frame_mutex_;
    std::vector<uint8_t> latest_frame_;
    UINT32 frame_width_  = 0;
    UINT32 frame_height_ = 0;

    bool running_   = false;
    bool on_battery_ = false;
};

} // namespace sr
