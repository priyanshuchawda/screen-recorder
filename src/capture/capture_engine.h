#pragma once
// capture_engine.h — Windows Graphics Capture (WGC) + GPU BGRA->NV12 conversion
// T010/T011: Captured frames converted to NV12 and pushed to BoundedQueue
// Uses PIMPL to keep WinRT types out of the header

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"

namespace sr {

using Microsoft::WRL::ComPtr;

// Video frame queue — spec mandates max 5 frames
using FrameQueue = BoundedQueue<RenderFrame, 5>;

// Forward declare the PIMPL impl class (defined in capture_engine.cpp)
struct CaptureEngineImpl;

class CaptureEngine {
    friend struct CaptureEngineImpl;  // allows PIMPL to access private counters
public:
    CaptureEngine();
    ~CaptureEngine();

    // Initialize:
    //   device/context — the D3D11 device to borrow for WGC interop and NV12 conversion
    //   queue          — output queue (owned by caller)
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, FrameQueue* queue);

    bool start();
    void stop();

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
    std::atomic<uint32_t> frames_captured_  { 0 };
    std::atomic<uint32_t> frames_dropped_   { 0 };
    int64_t               pts_anchor_100ns_ = 0;
    uint32_t              capture_width_    = 0;
    uint32_t              capture_height_   = 0;
};

} // namespace sr
