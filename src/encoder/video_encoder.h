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
#include <deque>
#include <cstdint>
#include <vector>
#include "utils/render_frame.h"
#include "encoder/deferred_input_queue.h"
#include "encoder/pending_input_accounting.h"

namespace sr {

using Microsoft::WRL::ComPtr;

enum class EncoderMode {
    HardwareMFT,        // Intel Quick Sync or other HW MFT
    SoftwareMFT,        // SW MFT, original resolution
    SoftwareMFT720p,    // SW MFT, 720p30 degraded fallback
};

class VideoEncoder {
public:
    struct DiagnosticsSnapshot {
        uint64_t input_samples_offered = 0;
        uint64_t input_samples_accepted = 0;
        uint64_t input_samples_not_accepted = 0;
        uint64_t input_samples_deferred_without_event = 0;
        uint64_t input_errors = 0;
        uint64_t output_samples_produced = 0;
        uint64_t output_waits_for_input = 0;
        uint64_t pending_inputs = 0;
        uint64_t pending_inputs_high_watermark = 0;
        uint64_t deferred_inputs_pending = 0;
        uint64_t deferred_inputs_high_watermark = 0;
        uint64_t pending_inputs_abandoned = 0;
        uint64_t input_samples_retried = 0;
        uint64_t output_timestamp_errors = 0;
    };

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
                      ComPtr<IMFSample>& out_sample,
                      std::shared_ptr<TextureSurfaceLease> surface_lease = {});

    // Drain remaining frames from encoder
    bool flush(std::vector<ComPtr<IMFSample>>& out_samples);

    void shutdown();

    // Request that the next encoded frame be a keyframe (IDR).
    // Call this immediately on resume from pause to ensure seekability.
    void request_keyframe() { force_keyframe_next_.store(true, std::memory_order_release); }

    EncoderMode  mode()         const { return published_mode_.load(std::memory_order_acquire); }
    uint32_t     output_width() const { return published_width_.load(std::memory_order_acquire); }
    uint32_t     output_height()const { return published_height_.load(std::memory_order_acquire); }
    uint32_t     output_fps()   const { return published_fps_.load(std::memory_order_acquire); }
    uint32_t     output_bitrate() const { return published_bitrate_.load(std::memory_order_acquire); }
    DiagnosticsSnapshot diagnostics_snapshot() const {
        return {
            input_samples_offered_.load(std::memory_order_relaxed),
            input_samples_accepted_.load(std::memory_order_relaxed),
            input_samples_not_accepted_.load(std::memory_order_relaxed),
            input_samples_deferred_without_event_.load(std::memory_order_relaxed),
            input_errors_.load(std::memory_order_relaxed),
            output_samples_produced_.load(std::memory_order_relaxed),
            output_waits_for_input_.load(std::memory_order_relaxed),
            pending_input_count_.load(std::memory_order_relaxed),
            pending_input_high_watermark_.load(std::memory_order_relaxed),
            deferred_input_count_.load(std::memory_order_relaxed),
            deferred_input_high_watermark_.load(std::memory_order_relaxed),
            pending_inputs_abandoned_.load(std::memory_order_relaxed),
            input_samples_retried_.load(std::memory_order_relaxed),
            output_timestamp_errors_.load(std::memory_order_relaxed)
        };
    }

private:
    bool try_init_hw(const EncoderProfile& profile, IMFDXGIDeviceManager* dxgi_mgr);
    bool try_init_sw(const EncoderProfile& profile, uint32_t width, uint32_t height, uint32_t fps);
    bool configure_encoder(IMFTransform* mft, uint32_t width, uint32_t height,
                           uint32_t fps, uint32_t bitrate_bps, bool use_hw);
    bool switch_to_software_fallback();
    bool pump_hw_events(uint32_t wait_ms);
    DeferredSubmitSummary submit_deferred_inputs();
    bool remove_deferred_input(IMFSample* sample);
    uint64_t abandon_deferred_inputs() noexcept;
    void update_deferred_input_metrics() noexcept;
    bool stamp_output_sample(IMFSample* sample, int64_t fallback_pts);
    void publish_configuration();

    ComPtr<IMFTransform>       mft_;
    ComPtr<IMFMediaEventGenerator> hw_events_;
    ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    ComPtr<ID3D11Device>       d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;

    EncoderMode mode_        = EncoderMode::SoftwareMFT;
    std::atomic<EncoderMode> published_mode_{ EncoderMode::SoftwareMFT };
    std::atomic<uint32_t> published_width_{ 1920 };
    std::atomic<uint32_t> published_height_{ 1080 };
    std::atomic<uint32_t> published_fps_{ 30 };
    std::atomic<uint32_t> published_bitrate_{ 4'000'000 };
    uint32_t    out_width_   = 1920;
    uint32_t    out_height_  = 1080;
    uint32_t    out_fps_     = 30;
    bool        initialized_ = false;
    bool        mft_provides_output_samples_ = true;
    uint32_t    mft_output_sample_size_      = 1 << 20;
    uint32_t    active_bitrate_bps_          = 4'000'000;

    // For HW path: need staging texture to share with MFT
    bool        hw_path_     = false;
    uint32_t    hw_output_fail_count_ = 0;
    bool        switched_to_sw_due_to_hw_errors_ = false;
    bool        hw_async_mft_ = false;
    uint32_t    hw_need_input_events_ = 0;
    uint32_t    hw_have_output_events_ = 0;
    bool        hw_drain_complete_ = false;
    ULONGLONG   last_output_tick_ms_ = 0;
    struct DeferredInput {
        ComPtr<IMFSample> sample;
        int64_t pts = 0;
        bool was_deferred = false;
    };
    static constexpr size_t kDeferredInputCapacity = 4;
    DeferredInputQueue<DeferredInput, kDeferredInputCapacity> deferred_inputs_;
    std::atomic<uint64_t> deferred_input_count_{ 0 };
    std::atomic<uint64_t> deferred_input_high_watermark_{ 0 };
    std::deque<int64_t> pending_video_pts_;
    std::atomic<uint64_t> pending_input_count_{ 0 };
    std::atomic<uint64_t> pending_input_high_watermark_{ 0 };
    std::atomic<uint64_t> pending_inputs_abandoned_{ 0 };

    // Session counters distinguish accepted MFT input from produced output.
    // A submitted input may legitimately produce output on a later call.
    std::atomic<uint64_t> input_samples_offered_{ 0 };
    std::atomic<uint64_t> input_samples_accepted_{ 0 };
    std::atomic<uint64_t> input_samples_not_accepted_{ 0 };
    std::atomic<uint64_t> input_samples_deferred_without_event_{ 0 };
    std::atomic<uint64_t> input_errors_{ 0 };
    std::atomic<uint64_t> output_samples_produced_{ 0 };
    std::atomic<uint64_t> output_waits_for_input_{ 0 };
    std::atomic<uint64_t> input_samples_retried_{ 0 };
    std::atomic<uint64_t> output_timestamp_errors_{ 0 };

    // Pre-allocated staging texture for SW encoder path (avoids per-frame alloc)
    ComPtr<ID3D11Texture2D> staging_tex_;
    uint32_t    staging_width_  = 0;
    uint32_t    staging_height_ = 0;
    bool ensure_staging_texture(uint32_t width, uint32_t height);

    // Force next frame to be an IDR keyframe (set on resume from pause)
    std::atomic<bool> force_keyframe_next_{ false };
};

} // namespace sr
