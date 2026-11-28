#pragma once
// session_controller.h — Wires all engines together; driven by SessionMachine events
// T016: Start initializes all engines, runs capture->preprocess->encode->mux pipeline,
//       Stop finalizes and renames file, Pause/Resume propagates to sync manager.
// T037: TelemetryStore integration for live counter snapshot
// T038: FramePacer for jitter absorption in encode_loop
// T039: Device-lost callback routing
// T042: PowerModeDetector applied at session start

#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <string>
#include "controller/session_machine.h"
#include "controller/recording_outcome.h"
#include "encoder/encoder_probe.h"
#include "encoder/power_mode.h"      // T042
#include "sync/sync_manager.h"
#include "sync/frame_pacer.h"        // T038
#include "app/telemetry.h"           // T037
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"
#include "utils/session_diagnostics.h"
#include "utils/latency_histogram.h"
#include "capture/capture_engine.h"  // for FrameQueue typedef
#include "audio/audio_engine.h"     // for AudioQueue typedef
#include "audio/audio_mixer.h"

namespace sr {

// Forward declarations
class CaptureEngine;
class AudioEngine;
class VideoEncoder;
class MuxWriter;
class StorageManager;

// Callback for UI status updates
using StatusCallback = std::function<void(const std::wstring& status)>;
using ErrorCallback  = std::function<void(const std::wstring& error)>;
using StopRequestCallback = std::function<void()>;

class SessionController {
public:
    SessionController();
    ~SessionController();

    // One-time setup; must be called before any Start
    bool initialize(StorageManager* storage,
                    StatusCallback on_status = nullptr,
                    ErrorCallback  on_error  = nullptr,
                    StopRequestCallback on_stop_requested = nullptr);

    // Start recording — transitions state machine Idle->Recording
    bool start();

    // Stop recording — transitions Recording/Paused->Stopping->Idle
    bool stop();
    // Last finalized recording result; stop() reports shutdown completion only.
    RecordingOutcome last_recording_outcome() const {
        return last_recording_outcome_.load(std::memory_order_acquire);
    }

    // Pause — Recording->Paused
    bool pause();

    // Resume — Paused->Recording
    bool resume();

    // Override encoder profile (fps/bitrate/resolution) — must be called before start()
    void set_encoder_profile(const EncoderProfile& profile, bool high_quality = false) {
        pending_profile_ = profile;
        pending_profile_high_quality_ = high_quality;
        has_pending_profile_ = true;
    }

    // Mute/Unmute audio
    void set_muted(bool muted);
    bool is_muted() const;

    // Current state
    SessionState state() const { return machine_.state(); }
    bool is_recording()   const { return machine_.is_recording(); }
    bool is_paused()      const { return machine_.is_paused(); }
    bool state_is_idle()  const { return machine_.is_idle(); }

    // Live stats (safe to read from UI thread)
    uint32_t frames_captured()  const;
    uint32_t frames_dropped()   const;
    uint32_t frames_encoded()   const;
    uint32_t audio_packets_written() const;
    uint32_t active_fps() const;
    EncoderProfile active_encoder_profile() const;

    // T037: Rich telemetry snapshot for debug overlay
    TelemetrySnapshot telemetry_snapshot() const;

    std::wstring output_path() const { return current_output_path_; }

private:
    struct StageTimingSnapshot {
        uint64_t samples = 0;
        uint64_t total_us = 0;
        uint64_t max_us = 0;
        uint64_t p50_upper_us = 0;
        uint64_t p95_upper_us = 0;
        uint64_t p99_upper_us = 0;
    };

    struct StageTimingCounters {
        void record(uint64_t duration_us) {
            samples.fetch_add(1, std::memory_order_relaxed);
            total_us.fetch_add(duration_us, std::memory_order_relaxed);
            histogram.record(duration_us);
            uint64_t current_max = max_us.load(std::memory_order_relaxed);
            while (duration_us > current_max &&
                   !max_us.compare_exchange_weak(current_max, duration_us,
                                                 std::memory_order_relaxed)) {}
        }
        StageTimingSnapshot snapshot() const {
            const auto percentiles = histogram.snapshot();
            return { samples.load(std::memory_order_relaxed),
                     total_us.load(std::memory_order_relaxed),
                     max_us.load(std::memory_order_relaxed),
                     percentiles.p50_upper_us,
                     percentiles.p95_upper_us,
                     percentiles.p99_upper_us };
        }
        void reset() {
            samples.store(0, std::memory_order_relaxed);
            total_us.store(0, std::memory_order_relaxed);
            max_us.store(0, std::memory_order_relaxed);
            histogram.reset();
        }
        std::atomic<uint64_t> samples{ 0 };
        std::atomic<uint64_t> total_us{ 0 };
        std::atomic<uint64_t> max_us{ 0 };
        LatencyHistogram histogram;
    };

    enum class DiagnosticsStage : uint32_t {
        Starting,
        WaitingForFrame,
        EncodingVideo,
        MuxingVideo,
        MixingAudio,
        MuxingAudio,
        StoppingDiskMonitor,
        StoppingCapture,
        StoppingMicrophone,
        StoppingLoopback,
        JoiningEncodeThread,
        FlushingEncoder,
        FinalizingMp4,
        Complete
    };

    // Encode/mux pipeline — runs on encode_thread_
    void encode_loop();
    static const wchar_t* diagnostics_stage_label(DiagnosticsStage stage);
    void start_diagnostics_monitor();
    void stop_diagnostics_monitor();
    void diagnostics_monitor_loop();
    void rollback_startup(const wchar_t* reason);

    // Shared state
    SessionMachine  machine_;
    SyncManager     sync_;
    ProbeResult     probe_;
    FixedFramePacer pacer_;      // M4: one authoritative fixed-FPS output clock
    TelemetryStore  telemetry_;  // T037: live counters
    std::atomic<bool> last_power_ac_{ true }; // T042: published power source state
    bool            active_high_quality_ = false;
    bool            mf_initialized_ = false;

    // Engines
    std::unique_ptr<CaptureEngine> capture_;
    std::unique_ptr<AudioEngine>   audio_;           // microphone
    std::unique_ptr<AudioEngine>   loopback_audio_;  // system/desktop audio
    std::unique_ptr<VideoEncoder>  encoder_;
    std::unique_ptr<MuxWriter>     muxer_;
    StorageManager*                storage_ = nullptr;

    // Queues shared between capture/audio producers and encode consumer
    PipelineWakeSignal pipeline_wake_;
    std::unique_ptr<FrameQueue> frame_queue_;
    std::unique_ptr<AudioQueue> audio_queue_;      // microphone
    std::unique_ptr<AudioQueue> loopback_queue_;   // system audio

    // Encode thread
    std::thread       encode_thread_;
    std::atomic<bool> encode_running_{ false };
    std::atomic<bool> pacer_reset_requested_{ false };
    std::atomic<uint64_t> scheduled_video_frames_{ 0 };
    std::atomic<int64_t> first_scheduled_video_pts_{ -1 };
    std::atomic<int64_t> last_scheduled_video_pts_{ -1 };

    // Low-frequency independent heartbeat keeps recording/stop diagnostics
    // useful even if the capture or encode thread stalls.
    std::thread diagnostics_thread_;
    std::atomic<bool> diagnostics_monitor_running_{ false };
    std::atomic<DiagnosticsStage> diagnostics_stage_{ DiagnosticsStage::Starting };
    std::mutex diagnostics_wait_mutex_;
    std::condition_variable diagnostics_wait_cv_;
    std::chrono::steady_clock::time_point diagnostics_started_{};

    // Counters (legacy — kept for backwards-compat; TelemetryStore is canonical)
    std::atomic<uint32_t> frames_encoded_{ 0 };
    std::atomic<uint32_t> video_write_failures_{ 0 };
    std::atomic<uint32_t> audio_write_failures_{ 0 };
    std::atomic<uint32_t> audio_source_start_failures_{ 0 };
    AudioMixerStats audio_mixer_stats_{}; // written by encode worker; read after join
    std::atomic<bool> output_failure_reported_{ false };
    std::atomic<bool> stopping_from_pause_{ false };
    std::atomic<uint32_t> pipeline_stall_events_{ 0 };
    std::atomic<RecordingOutcome> last_recording_outcome_{ RecordingOutcome::NotRun };
    std::atomic<uint32_t> audio_written_ { 0 };
    uint64_t diagnostic_log_dropped_at_start_ = 0;
    StageTimingCounters video_encode_timing_;
    StageTimingCounters video_mux_timing_;
    StageTimingCounters audio_mux_timing_;

    // DXGI process usage/budget snapshots, refreshed by the independent
    // diagnostics monitor so encoder stalls do not stop memory sampling.
    std::atomic<bool> gpu_memory_sample_available_{ false };
    std::atomic<uint64_t> gpu_memory_sample_tick_ms_{ 0 };
    std::atomic<uint64_t> gpu_local_usage_bytes_{ 0 };
    std::atomic<uint64_t> gpu_local_budget_bytes_{ 0 };
    std::atomic<uint64_t> gpu_nonlocal_usage_bytes_{ 0 };
    std::atomic<uint64_t> gpu_nonlocal_budget_bytes_{ 0 };

    // Output path for current recording
    std::wstring current_output_path_;
    std::wstring current_partial_path_;
    SessionDiagnostics diagnostics_;

    // Optional encoder profile override (set via set_encoder_profile before start)
    EncoderProfile pending_profile_;
    bool           pending_profile_high_quality_ = false;
    bool           has_pending_profile_ = false;

    // Callbacks
    StatusCallback on_status_;
    ErrorCallback  on_error_;
    StopRequestCallback on_stop_requested_;

    void notify_status(const std::wstring& msg);
    void notify_error (const std::wstring& msg);
    void report_recording_loss(const wchar_t* stage);
};

} // namespace sr
