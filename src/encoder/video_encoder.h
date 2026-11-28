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
#include <memory>
#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>
#include "utils/render_frame.h"
#include "utils/pipeline_wake_signal.h"
#include "encoder/pending_input_accounting.h"
#include "encoder/async_mft_scheduler.h"
#include "encoder/async_mft_callback.h"
#include "utils/latency_histogram.h"

namespace sr {

using Microsoft::WRL::ComPtr;

enum class EncoderMode {
    HardwareMFT,        // Intel Quick Sync or other HW MFT
    SoftwareMFT,        // SW MFT, original resolution
    SoftwareMFT720p,    // SW MFT, 720p30 degraded fallback
};

class VideoEncoder {
public:
    struct AsyncEncodedOutput;

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
        uint64_t input_preparation_errors = 0;
        uint64_t output_errors = 0;
        uint64_t scheduler_errors = 0;
        uint64_t input_accounting_errors = 0;
        uint64_t async_need_input_events = 0;
        uint64_t async_have_output_events = 0;
        uint64_t async_worker_wakeups = 0;
        uint64_t async_output_queue_high_watermark = 0;
        uint64_t async_output_buffer_capacity_high_watermark = 0;
        uint64_t async_input_credit_high_watermark = 0;
        LatencyHistogram::Snapshot async_event_wait_us{};
        LatencyHistogram::Snapshot async_input_queue_residence_us{};
        LatencyHistogram::Snapshot async_process_input_us{};
        LatencyHistogram::Snapshot async_process_output_us{};
        LatencyHistogram::Snapshot async_accept_to_output_us{};
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
                      std::shared_ptr<TextureSurfaceLease> surface_lease = {},
                      bool synthetic = false);

    // Drain remaining frames from encoder
    bool flush(std::vector<AsyncEncodedOutput>& out_samples);

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
        DiagnosticsSnapshot snapshot{
            input_samples_offered_.load(std::memory_order_relaxed),
            input_samples_accepted_.load(std::memory_order_relaxed),
            input_samples_not_accepted_.load(std::memory_order_relaxed),
            input_samples_deferred_without_event_.load(std::memory_order_relaxed),
            input_errors_.load(std::memory_order_relaxed),
            output_samples_produced_.load(std::memory_order_relaxed),
            output_waits_for_input_.load(std::memory_order_relaxed),
            pending_input_count_.load(std::memory_order_relaxed),
            pending_input_high_watermark_.load(std::memory_order_relaxed),
            async_scheduler_->input_count(),
            async_scheduler_->input_queue_high_water(),
            pending_inputs_abandoned_.load(std::memory_order_relaxed),
            input_samples_retried_.load(std::memory_order_relaxed),
            output_timestamp_errors_.load(std::memory_order_relaxed),
            input_preparation_errors_.load(std::memory_order_relaxed),
            output_errors_.load(std::memory_order_relaxed),
            scheduler_errors_.load(std::memory_order_relaxed),
            input_accounting_errors_.load(std::memory_order_relaxed)
        };
        snapshot.async_need_input_events = async_scheduler_->need_input_events_received();
        snapshot.async_have_output_events = async_scheduler_->have_output_events_received();
        snapshot.async_worker_wakeups = async_worker_wakeups_.load(std::memory_order_relaxed);
        snapshot.async_output_queue_high_watermark = async_scheduler_->output_high_water();
        snapshot.async_output_buffer_capacity_high_watermark = async_scheduler_->output_bytes_high_water();
        snapshot.async_input_credit_high_watermark = async_scheduler_->input_credit_high_water();
        snapshot.async_event_wait_us = async_event_wait_us_.snapshot();
        snapshot.async_input_queue_residence_us = async_input_queue_residence_us_.snapshot();
        snapshot.async_process_input_us = async_process_input_us_.snapshot();
        snapshot.async_process_output_us = async_process_output_us_.snapshot();
        snapshot.async_accept_to_output_us = async_accept_to_output_us_.snapshot();
        return snapshot;
    }

    struct AsyncEncodedOutput {
        ComPtr<IMFSample> sample;
        uint64_t frame_id = 0;
        int64_t pts = 0;
        bool synthetic = false;
    };

    bool try_pop_async_output(AsyncEncodedOutput& output);
    bool async_worker_failed() const { return hw_async_mft_ && async_scheduler_->failed(); }
    void set_pipeline_wake_signal(const std::shared_ptr<PipelineWakeSignal>& signal) {
        pipeline_wake_signal_ = signal;
    }

private:
    struct DeferredInput {
        ComPtr<IMFSample> sample;
        int64_t pts = 0;
        uint64_t frame_id = 0;
        bool synthetic = false;
        bool force_keyframe = false;
        bool was_deferred = false;
        std::chrono::steady_clock::time_point queued_at{};
    };
    using AsyncScheduler = AsyncMftScheduler<DeferredInput, AsyncEncodedOutput>;

    bool try_init_hw(const EncoderProfile& profile, IMFDXGIDeviceManager* dxgi_mgr);
    bool try_init_sw(const EncoderProfile& profile, uint32_t width, uint32_t height, uint32_t fps);
    bool configure_encoder(IMFTransform* mft, uint32_t width, uint32_t height,
                           uint32_t fps, uint32_t bitrate_bps, bool use_hw);
    bool switch_to_software_fallback();
    bool start_async_worker();
    void async_worker_loop();
    bool async_drain_to(std::vector<AsyncEncodedOutput>& out_samples, DWORD timeout_ms);
    bool stamp_output_sample(IMFSample* sample, int64_t fallback_pts);
    void publish_configuration();
    void notify_pipeline(PipelineWakeSource source);

    ComPtr<IMFTransform>       mft_;
    ComPtr<IMFMediaEventGenerator> hw_events_;
    ComPtr<MftAsyncEventCallback<AsyncScheduler>> hw_event_callback_;
    ComPtr<IMFShutdown> hw_shutdown_;
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
    uint32_t    mft_output_sample_alignment_ = MF_1_BYTE_ALIGNMENT;
    uint32_t    active_bitrate_bps_          = 4'000'000;

    // For HW path: need staging texture to share with MFT
    bool        hw_path_     = false;
    uint32_t    hw_output_fail_count_ = 0;
    bool        switched_to_sw_due_to_hw_errors_ = false;
    bool        hw_async_mft_ = false;
    std::shared_ptr<AsyncScheduler> async_scheduler_ = std::make_shared<AsyncScheduler>(
        kAsyncMftInputQueueCapacity,
        kAsyncMftMaxInFlightInputs,
        kAsyncMftOutputQueueCapacity,
        kAsyncMftOutputQueueByteCapacity
    );
    std::weak_ptr<PipelineWakeSignal> pipeline_wake_signal_;
    std::thread async_worker_;
    std::atomic<bool> async_worker_stopping_{ false };
    std::atomic<uint64_t> async_worker_wakeups_{ 0 };
    LatencyHistogram async_event_wait_us_;
    LatencyHistogram async_input_queue_residence_us_;
    LatencyHistogram async_process_input_us_;
    LatencyHistogram async_process_output_us_;
    LatencyHistogram async_accept_to_output_us_;
    struct AcceptedFrameMetadata {
        uint64_t frame_id = 0;
        int64_t pts = 0;
        bool synthetic = false;
        std::chrono::steady_clock::time_point accepted_at{};
    };
    std::deque<AcceptedFrameMetadata> async_pending_frames_;
    std::atomic<ULONGLONG> last_output_tick_ms_{ 0 };
    std::atomic<uint64_t> next_frame_id_{ 0 };
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
    std::atomic<uint64_t> input_preparation_errors_{ 0 };
    std::atomic<uint64_t> output_errors_{ 0 };
    std::atomic<uint64_t> scheduler_errors_{ 0 };
    std::atomic<uint64_t> input_accounting_errors_{ 0 };

    // Pre-allocated staging texture for SW encoder path (avoids per-frame alloc)
    ComPtr<ID3D11Texture2D> staging_tex_;
    uint32_t    staging_width_  = 0;
    uint32_t    staging_height_ = 0;
    bool ensure_staging_texture(uint32_t width, uint32_t height);

    // Force next frame to be an IDR keyframe (set on resume from pause)
    std::atomic<bool> force_keyframe_next_{ false };
};

} // namespace sr
