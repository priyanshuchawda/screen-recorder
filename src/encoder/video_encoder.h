#pragma once
// video_encoder.h — Media Foundation H.264 hardware encoder with 3-step fallback chain
// T013: CBR, 2s GOP, low-latency, no B-frames. Baseline/Main profile.
// Fallback chain: HW MFT -> SW MFT (same res) -> 720p30 SW

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include "utils/render_frame.h"

namespace sr {

using Microsoft::WRL::ComPtr;

enum class EncoderMode {
    HardwareMFT,        // Intel Quick Sync or other HW MFT
    SoftwareMFT,        // SW MFT, original resolution
    SoftwareMFT720p,    // SW MFT, 720p30 degraded fallback
};

class VideoEncoder {
public:
    VideoEncoder()  = default;
    ~VideoEncoder() { shutdown(); }

    // Initialize with an EncoderProfile and an optional DXGI device manager for HW path.
    // If dxgi_mgr is null, skips HW encoder attempt.
    bool initialize(const EncoderProfile& profile, IMFDXGIDeviceManager* dxgi_mgr,
                    ID3D11Device* d3d_device, ID3D11DeviceContext* d3d_context);

    // Encode one NV12 frame.  pts is in 100ns units.
    // Encoded bytes are appended to out_samples (as IMFSample AddRef'd pointers).
    // Caller must Release() each sample after writing it to the muxer.
    bool encode_frame(ID3D11Texture2D* nv12_texture, int64_t pts,
                      ComPtr<IMFSample>& out_sample);

    // Drain remaining frames from encoder
    bool flush(std::vector<ComPtr<IMFSample>>& out_samples);

    void shutdown();

    // Request that the next encoded frame be a keyframe (IDR).
    // Call this immediately on resume from pause to ensure seekability.
    void request_keyframe() { force_keyframe_next_.store(true, std::memory_order_release); }

    EncoderMode  mode()         const { return mode_; }
    uint32_t     output_width() const { return out_width_; }
    uint32_t     output_height()const { return out_height_; }
    uint32_t     output_fps()   const { return out_fps_; }

private:
    bool try_init_hw(const EncoderProfile& profile, IMFDXGIDeviceManager* dxgi_mgr);
    bool try_init_sw(const EncoderProfile& profile, uint32_t width, uint32_t height, uint32_t fps);
    bool configure_encoder(IMFTransform* mft, uint32_t width, uint32_t height,
                           uint32_t fps, uint32_t bitrate_bps, bool use_hw);

    ComPtr<IMFTransform>       mft_;
    ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    ComPtr<ID3D11Device>       d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;

    EncoderMode mode_        = EncoderMode::SoftwareMFT;
    uint32_t    out_width_   = 1920;
    uint32_t    out_height_  = 1080;
    uint32_t    out_fps_     = 30;
    bool        initialized_ = false;
    bool        mft_provides_output_samples_ = true;
    uint32_t    mft_output_sample_size_      = 1 << 20;

    // For HW path: need staging texture to share with MFT
    bool        hw_path_     = false;

    // Force next frame to be an IDR keyframe (set on resume from pause)
    std::atomic<bool> force_keyframe_next_{ false };
};

} // namespace sr
