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

namespace sr {

using Microsoft::WRL::ComPtr;

// Video frame queue — spec mandates max 5 frames
using FrameQueue = BoundedQueue<RenderFrame, 5>;

// Forward declare the PIMPL impl class (defined in capture_engine.cpp)
struct CaptureEngineImpl;

// Callback fired on DXGI_ERROR_DEVICE_REMOVED or DXGI_ERROR_DEVICE_RESET.
// Invoked from the WGC frame-arrived callback thread — must be thread-safe.
using DeviceLostCallback = std::function<void()>;

class CaptureEngine {
    friend struct CaptureEngineImpl;  // allows PIMPL to access private counters
public:
    CaptureEngine();
    ~CaptureEngine();

    // T043: Check whether Windows Graphics Capture is supported on this machine.
    // Returns false on Windows < 10 build 1803 or when WGC components are missing.
    static bool is_wgc_supported();

    // Initialize:
    //   device/context — the D3D11 device to borrow for WGC interop and NV12 conversion
    //   queue          — output queue (owned by caller)
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, FrameQueue* queue);

    bool start();
    void stop();

    // T039: Register a callback fired when the D3D11 device is lost.
    // The controller should stop capture and optionally attempt re-initialization.
    void set_device_lost_callback(DeviceLostCallback cb) { device_lost_cb_ = std::move(cb); }

    // Live counters (thread-safe reads)
    uint32_t frames_captured() const { return frames_captured_.load(std::memory_order_relaxed); }
    uint32_t frames_dropped()  const { return frames_dropped_.load(std::memory_order_relaxed); }

    // Set QPC-based PTS anchor (call between initialize and start)
    void set_sync_anchor_100ns(int64_t anchor) { pts_anchor_100ns_ = anchor; }

    // Capture dimensions (valid after initialize)
    uint32_t width()  const { return capture_width_; }
    uint32_t height() const { return capture_height_; }

private:
    std::unique_ptr<CaptureEngineImpl> impl_;

    std::atomic<bool>     running_          { false };
    std::atomic<bool>     device_lost_      { false }; // T039: set when DXGI device removed
    std::atomic<uint32_t> frames_captured_  { 0 };
    std::atomic<uint32_t> frames_dropped_   { 0 };
    int64_t               pts_anchor_100ns_ = 0;
    uint32_t              capture_width_    = 0;
    uint32_t              capture_height_   = 0;
    DeviceLostCallback    device_lost_cb_;  // T039
};

} // namespace sr
