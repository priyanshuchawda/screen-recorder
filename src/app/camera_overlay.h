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
    void set_high_quality(bool enabled);

    bool is_running() const { return running_; }

    static constexpr UINT32 kEfficiencyPreviewMaxWidth = 640;
    static constexpr UINT32 kEfficiencyPreviewMaxHeight = 480;
    static constexpr UINT32 kEfficiencyPreviewPreferredWidth = 640;
    static constexpr UINT32 kEfficiencyPreviewPreferredHeight = 360;
    static constexpr UINT32 kHighQualityPreviewMaxWidth = 1280;
    static constexpr UINT32 kHighQualityPreviewMaxHeight = 720;
    static constexpr UINT32 kHighQualityPreviewPreferredWidth = 1280;
    static constexpr UINT32 kHighQualityPreviewPreferredHeight = 720;
    static int preview_interval_ms_for_power(bool on_battery) {
        return preview_interval_ms_for_profile(on_battery, false);
    }
    static bool use_high_quality_preview(bool on_battery, bool high_quality) {
        return high_quality && !on_battery;
    }
    static int preview_interval_ms_for_profile(bool on_battery, bool high_quality) {
        if (on_battery) return 100; // 10 fps on battery
        return high_quality ? 33 : 66; // ~30 fps HQ on AC, ~15 fps default on AC
    }
    static UINT32 preview_max_width_for_profile(bool on_battery, bool high_quality) {
        return use_high_quality_preview(on_battery, high_quality)
            ? kHighQualityPreviewMaxWidth
            : kEfficiencyPreviewMaxWidth;
    }
    static UINT32 preview_max_height_for_profile(bool on_battery, bool high_quality) {
        return use_high_quality_preview(on_battery, high_quality)
            ? kHighQualityPreviewMaxHeight
            : kEfficiencyPreviewMaxHeight;
    }
    static UINT32 preview_preferred_width_for_profile(bool on_battery, bool high_quality) {
        return use_high_quality_preview(on_battery, high_quality)
            ? kHighQualityPreviewPreferredWidth
            : kEfficiencyPreviewPreferredWidth;
    }
    static UINT32 preview_preferred_height_for_profile(bool on_battery, bool high_quality) {
        return use_high_quality_preview(on_battery, high_quality)
            ? kHighQualityPreviewPreferredHeight
            : kEfficiencyPreviewPreferredHeight;
    }
    static bool should_process_preview_frame(unsigned long long now_ms,
                                             unsigned long long last_processed_ms,
                                             int interval_ms) {
        return interval_ms <= 0 ||
               last_processed_ms == 0 ||
               now_ms - last_processed_ms >= static_cast<unsigned long long>(interval_ms);
    }

private:
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void capture_loop();
    void draw_latest_frame(HDC hdc, const RECT& rc);
    void resize_capture_to_client();
    void stop_capture_thread();

    bool detect_on_battery() const;
    void apply_preview_tuning();

    HWND owner_ = nullptr;
    HWND host_hwnd_ = nullptr;

    std::thread capture_thread_;
    std::atomic<bool> capture_running_{ false };
    std::atomic<int>  capture_interval_ms_{ 16 };
    std::atomic<bool> high_quality_{ false };

    std::mutex frame_mutex_;
    std::vector<uint8_t> latest_frame_;
    UINT32 frame_width_  = 0;
    UINT32 frame_height_ = 0;

    bool running_   = false;
    bool on_battery_ = false;
};

} // namespace sr
