#pragma once
// capture_engine.h — Windows Graphics Capture (WGC) + GPU BGRA->NV12 conversion
// T010/T011: Captured frames converted to NV12 and pushed to BoundedQueue
// T039: Device-lost callback for DXGI_ERROR_DEVICE_REMOVED recovery
// T043: WGC availability check + consent error reporting
// Uses PIMPL to keep WinRT types out of the header

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <memory>
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"
#include "utils/latency_histogram.h"

namespace sr {

class SyncManager;

using Microsoft::WRL::ComPtr;

// Short bounded backlog absorbs brief encoder scheduling stalls.
// At 30 fps, eight frames represent about 267 ms without unbounded latency.
using FrameQueue = BoundedQueue<RenderFrame, 8>;

// Forward declare the PIMPL impl class (defined in capture_engine.cpp)
struct CaptureEngineImpl;

// Callback fired on DXGI_ERROR_DEVICE_REMOVED or DXGI_ERROR_DEVICE_RESET.
// Invoked from the WGC frame-arrived callback thread — must be thread-safe.
using DeviceLostCallback = std::function<void()>;

class CaptureEngine {
    friend struct CaptureEngineImpl;  // allows PIMPL to access private counters
public:
    enum class FrameCallbackStage : uint32_t {
        Idle,
        GetFrame,
        ReadFrameMetadata,
        ReadSurface,
        ConvertGpu,
        QueueFrame
    };

    CaptureEngine();
    ~CaptureEngine();

    // T043: Check whether Windows Graphics Capture is supported on this machine.
    // Returns false on Windows < 10 build 1803 or when WGC components are missing.
    static bool is_wgc_supported();

    // Initialize:
    //   device/context — the D3D11 device to borrow for WGC interop and NV12 conversion
    //   queue          — output queue (owned by caller)
    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    FrameQueue* queue,
                    RecordingResolution max_resolution = kEfficiencyRecordingResolution,
                    uint32_t target_fps = 30);
    void set_target_fps(uint32_t fps);
    uint32_t target_fps() const { return target_fps_.load(std::memory_order_relaxed); }

    bool start();
    void stop();

    // T039: Register a callback fired when the D3D11 device is lost.
    // The controller should stop capture and optionally attempt re-initialization.
    void set_device_lost_callback(DeviceLostCallback cb) { device_lost_cb_ = std::move(cb); }

    // Live counters (thread-safe reads)
    uint32_t frames_captured() const { return frames_captured_.load(std::memory_order_relaxed); }
    uint32_t frames_dropped()  const { return frames_dropped_.load(std::memory_order_relaxed); }
    uint32_t resize_failures() const { return resize_failures_.load(std::memory_order_relaxed); }
    uint32_t texture_acquisition_failures() const { return texture_acquisition_failures_.load(std::memory_order_relaxed); }
    uint32_t queue_drops()     const { return queue_drops_.load(std::memory_order_relaxed); }
    uint32_t cadence_rejections() const { return cadence_rejections_.load(std::memory_order_relaxed); }
    uint64_t conversion_submission_samples() const { return conversion_submission_samples_.load(std::memory_order_relaxed); }
    uint64_t conversion_submission_total_us() const { return conversion_submission_total_us_.load(std::memory_order_relaxed); }
    uint64_t conversion_submission_max_us() const { return conversion_submission_max_us_.load(std::memory_order_relaxed); }
    LatencyHistogram::Snapshot conversion_submission_latency() const { return conversion_submission_latency_.snapshot(); }
    FrameCallbackStage frame_callback_stage() const {
        return frame_callback_stage_.load(std::memory_order_relaxed);
    }
    uint64_t frame_callback_age_ms() const {
        const uint64_t started = frame_callback_started_ms_.load(std::memory_order_relaxed);
        return started ? GetTickCount64() - started : 0;
    }
    void reset_counters() {
        frames_captured_.store(0, std::memory_order_relaxed);
        frames_dropped_.store(0, std::memory_order_relaxed);
        resize_failures_.store(0, std::memory_order_relaxed);
        texture_acquisition_failures_.store(0, std::memory_order_relaxed);
        queue_drops_.store(0, std::memory_order_relaxed);
        cadence_rejections_.store(0, std::memory_order_relaxed);
        conversion_submission_samples_.store(0, std::memory_order_relaxed);
        conversion_submission_total_us_.store(0, std::memory_order_relaxed);
        conversion_submission_max_us_.store(0, std::memory_order_relaxed);
        conversion_submission_latency_.reset();
    }

    // Set QPC-based PTS anchor (call between initialize and start)
    void set_sync_anchor_100ns(int64_t anchor) { pts_anchor_100ns_ = anchor; }
    void set_sync_manager(SyncManager* sync) { sync_manager_ = sync; }

    // Capture dimensions (valid after initialize)
    uint32_t width()  const { return capture_width_; }
    uint32_t height() const { return capture_height_; }

private:
    void set_frame_callback_stage(FrameCallbackStage stage) {
        if (stage != FrameCallbackStage::Idle) {
            frame_callback_started_ms_.store(GetTickCount64(), std::memory_order_relaxed);
        } else {
            frame_callback_started_ms_.store(0, std::memory_order_relaxed);
        }
        frame_callback_stage_.store(stage, std::memory_order_relaxed);
    }

    std::unique_ptr<CaptureEngineImpl> impl_;

    std::atomic<bool>     running_          { false };
    std::atomic<bool>     device_lost_      { false }; // T039: set when DXGI device removed
    std::atomic<uint32_t> frames_captured_  { 0 };
    std::atomic<uint32_t> frames_dropped_   { 0 };
    std::atomic<uint32_t> resize_failures_{ 0 };
    std::atomic<uint32_t> texture_acquisition_failures_{ 0 };
    std::atomic<uint32_t> queue_drops_      { 0 };
    std::atomic<uint32_t> cadence_rejections_{ 0 };
    std::atomic<uint64_t> conversion_submission_samples_{ 0 };
    std::atomic<uint64_t> conversion_submission_total_us_{ 0 };
    std::atomic<uint64_t> conversion_submission_max_us_{ 0 };
    LatencyHistogram conversion_submission_latency_;
    std::atomic<FrameCallbackStage> frame_callback_stage_{ FrameCallbackStage::Idle };
    std::atomic<uint64_t> frame_callback_started_ms_{ 0 };
    std::atomic<uint32_t> target_fps_       { 30 };
    int64_t               pts_anchor_100ns_ = 0;
    SyncManager*          sync_manager_ = nullptr; // Owned by SessionController.
    uint32_t              capture_width_    = 0;
    uint32_t              capture_height_   = 0;
    DeviceLostCallback    device_lost_cb_;  // T039
};

} // namespace sr
