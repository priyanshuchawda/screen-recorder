// session_controller.cpp — Wires all engines together into the recording pipeline
// T016: Start -> init engines -> run capture->encode->mux loop -> Stop -> finalize

#include "controller/session_controller.h"
#include "controller/audio_write_schedule.h"
#include "controller/recording_outcome.h"
#include "capture/capture_engine.h"
#include "audio/audio_engine.h"
#include "audio/audio_mixer.h"
#include "encoder/video_encoder.h"
#include "storage/mux_writer.h"
#include "storage/storage_manager.h"
#include "utils/logging.h"
#include "utils/qpc_clock.h"
#include "audio/audio_sample_factory.h"
#include "utils/resource_metrics.h"

#include <mfapi.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <limits>
#include <system_error>
#include <dxgi1_4.h>
#include <psapi.h>

namespace sr {

namespace {

const wchar_t* encoder_mode_label(EncoderMode mode) {
    switch (mode) {
        case EncoderMode::HardwareMFT: return L"HW";
        case EncoderMode::SoftwareMFT: return L"SW";
        case EncoderMode::SoftwareMFT720p: return L"SW 720p";
        default: return L"?";
    }
}

const wchar_t* capture_stage_label(CaptureEngine::FrameCallbackStage stage) {
    switch (stage) {
        case CaptureEngine::FrameCallbackStage::Idle: return L"idle";
        case CaptureEngine::FrameCallbackStage::GetFrame: return L"get_frame";
        case CaptureEngine::FrameCallbackStage::ReadFrameMetadata: return L"frame_metadata";
        case CaptureEngine::FrameCallbackStage::ReadSurface: return L"frame_surface";
        case CaptureEngine::FrameCallbackStage::ConvertGpu: return L"video_processor_gpu";
        case CaptureEngine::FrameCallbackStage::QueueFrame: return L"queue_frame";
        default: return L"unknown";
    }
}

uint64_t filetime_value_100ns(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

bool process_cpu_time_100ns(uint64_t& cpu_time) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return false;
    }
    cpu_time = filetime_value_100ns(kernel) + filetime_value_100ns(user);
    return true;
}

const wchar_t* power_source_label(BYTE ac_line_status) {
    if (ac_line_status == 1) return L"AC";
    if (ac_line_status == 0) return L"battery";
    return L"unknown";
}

const wchar_t* battery_state_label(BYTE battery_flag) {
    if (battery_flag == 255) return L"unknown";
    if ((battery_flag & 128) != 0) return L"no_battery";
    if ((battery_flag & 8) != 0) return L"charging";
    return L"discharging";
}

} // namespace

SessionController::SessionController()
    : capture_(std::make_unique<CaptureEngine>())
    , audio_  (std::make_unique<AudioEngine>())
    , loopback_audio_(std::make_unique<AudioEngine>())
    , encoder_(std::make_unique<VideoEncoder>())
    , muxer_  (std::make_unique<MuxWriter>())
    , frame_queue_(std::make_unique<FrameQueue>())
    , audio_queue_(std::make_unique<AudioQueue>())
    , loopback_queue_(std::make_unique<AudioQueue>())
{}

SessionController::~SessionController() {
    stop_diagnostics_monitor();
    if (!machine_.is_idle()) {
        encode_running_.store(false, std::memory_order_release);
        capture_->stop();
        audio_->shutdown();
        loopback_audio_->shutdown();
        if (encode_thread_.joinable()) encode_thread_.join();
        encoder_->shutdown();
        muxer_->abort();
    } else {
        capture_->stop();
        audio_->shutdown();
        loopback_audio_->shutdown();
        encoder_->shutdown();
        muxer_->abort();
    }
    if (mf_initialized_) {
        probe_.dxgi_device_manager.Reset();
        MFShutdown();
        mf_initialized_ = false;
    }
}

void SessionController::rollback_startup(const wchar_t* reason) {
    // Claim the stopping transition before touching shared engines. This keeps
    // a concurrent disk-space auto-stop from tearing down the same resources.
    if (machine_.is_recording() || machine_.is_paused()) {
        if (!machine_.transition(SessionEvent::Stop)) return;
    } else if (machine_.is_stopping()) {
        return;
    }

    diagnostics_.write_failure(reason ? reason : L"recording_start_failed");
    diagnostics_.write_event(L"event=startup_rollback state=begin");
    diagnostics_stage_.store(DiagnosticsStage::StoppingCapture, std::memory_order_relaxed);

    if (storage_) storage_->stopDiskSpacePolling();
    capture_->stop();
    audio_->shutdown();
    loopback_audio_->shutdown();
    encode_running_.store(false, std::memory_order_release);
    if (encode_thread_.joinable()) encode_thread_.join();
    encoder_->shutdown();
    muxer_->abort(); // Keep any partial file; never relabel an unvalidated startup output.

    while (frame_queue_ && frame_queue_->try_pop()) {}
    while (audio_queue_ && audio_queue_->try_pop()) {}
    while (loopback_queue_ && loopback_queue_->try_pop()) {}

    stop_diagnostics_monitor();
    diagnostics_.write_event(L"event=startup_rollback state=complete");
    flush_persistent_log(std::chrono::seconds(2));
    clear_persistent_log_path();
    last_recording_outcome_.store(RecordingOutcome::FailedPartialKept,
                                  std::memory_order_release);
    if (machine_.is_stopping()) machine_.transition(SessionEvent::Finalized);
    encode_running_.store(false, std::memory_order_release);
    stopping_from_pause_.store(false, std::memory_order_release);
    notify_status(L"Idle");
}

const wchar_t* SessionController::diagnostics_stage_label(DiagnosticsStage stage) {
    switch (stage) {
        case DiagnosticsStage::Starting: return L"starting";
        case DiagnosticsStage::WaitingForFrame: return L"waiting_for_frame";
        case DiagnosticsStage::EncodingVideo: return L"video_encode";
        case DiagnosticsStage::MuxingVideo: return L"video_mux_write";
        case DiagnosticsStage::MixingAudio: return L"audio_mix";
        case DiagnosticsStage::MuxingAudio: return L"audio_mux_write";
        case DiagnosticsStage::StoppingDiskMonitor: return L"stop_disk_monitor";
        case DiagnosticsStage::StoppingCapture: return L"stop_capture";
        case DiagnosticsStage::StoppingMicrophone: return L"stop_microphone";
        case DiagnosticsStage::StoppingLoopback: return L"stop_loopback";
        case DiagnosticsStage::JoiningEncodeThread: return L"join_encode_thread";
        case DiagnosticsStage::FlushingEncoder: return L"encoder_flush";
        case DiagnosticsStage::FinalizingMp4: return L"finalize_mp4";
        case DiagnosticsStage::Complete: return L"complete";
        default: return L"unknown";
    }
}

bool SessionController::initialize(StorageManager* storage,
                                    StatusCallback on_status,
                                    ErrorCallback  on_error,
                                    StopRequestCallback on_stop_requested)
{
    storage_   = storage;
    on_status_ = std::move(on_status);
    on_error_  = std::move(on_error);
    on_stop_requested_ = std::move(on_stop_requested);

    // Initialize Media Foundation once per controller lifetime. Balance the
    // startup reference in the destructor, including probe-failure paths.
    if (!mf_initialized_) {
        const HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"MFStartup failed: 0x%08X", hr);
            return false;
        }
        mf_initialized_ = true;
    }

    // Probe D3D11 + HW encoder
    if (!EncoderProbe::run(probe_)) {
        notify_error(L"D3D11 initialization failed");
        probe_.dxgi_device_manager.Reset();
        MFShutdown();
        mf_initialized_ = false;
        return false;
    }

    SR_LOG_INFO(L"SessionController initialized. Adapter: %s, HW encoder: %s",
        probe_.adapter_name.c_str(),
        probe_.hw_encoder_available ? probe_.encoder_name.c_str() : L"not available"
    );
    return true;
}

bool SessionController::start() {
    // Join any completed prior-session monitor before allowing a new session.
    if (machine_.is_idle()) stop_diagnostics_monitor();
    if (!machine_.transition(SessionEvent::Start)) return false;
    last_recording_outcome_.store(RecordingOutcome::NotRun, std::memory_order_release);
    pipeline_stall_events_.store(0, std::memory_order_relaxed);

    diagnostics_stage_.store(DiagnosticsStage::Starting, std::memory_order_relaxed);

    notify_status(L"Starting...");

    // ---------------------------------------------------------------
    // Determine output paths
    // generateFilename() returns the .partial.mp4 path
    // ---------------------------------------------------------------
    current_partial_path_ = storage_ ? storage_->generateFilename() : L"ScreenRec.partial.mp4";
    current_output_path_  = StorageManager::partialToFinal(current_partial_path_);
    diagnostic_log_dropped_at_start_ = persistent_log_dropped_count();
    if (diagnostics_.open_for_output(current_output_path_)) {
        SR_LOG_INFO(L"Diagnostics log -> %s", diagnostics_.path().c_str());
    } else {
        SR_LOG_WARN(L"Diagnostics log could not be opened for: %s", current_output_path_.c_str());
    }

    // ---------------------------------------------------------------
    // Anchor sync clock
    // ---------------------------------------------------------------
    sync_.start();
    capture_->set_sync_manager(&sync_);
    audio_->set_sync_manager(&sync_);
    loopback_audio_->set_sync_manager(&sync_);

    // ---------------------------------------------------------------
    // Resolve requested recording profile before capture initialization.
    // This lets capture allocate only the texture size the active profile needs.
    // ---------------------------------------------------------------
    EncoderProfile enc_prof;
    bool high_quality_profile = false;
    if (has_pending_profile_) {
        enc_prof = pending_profile_;
        high_quality_profile = pending_profile_high_quality_;
        SR_LOG_INFO(L"Using custom encoder profile: %ufps, %u bps, %ux%u, high_quality=%s",
                    enc_prof.fps, enc_prof.bitrate_bps,
                    enc_prof.width, enc_prof.height,
                    high_quality_profile ? L"on" : L"off");
    } else {
        enc_prof.fps         = 30;
        enc_prof.bitrate_bps = 4'000'000;
        enc_prof.width       = kEfficiencyRecordingResolution.width;
        enc_prof.height      = kEfficiencyRecordingResolution.height;
    }

    // T042: Apply base-mode power clamping before capture allocates output textures.
    last_power_ac_.store(PowerModeDetector::is_on_ac_power(), std::memory_order_release);
    enc_prof = PowerModeDetector::clamp_for_quality_and_power_state(
        enc_prof, last_power_ac_.load(std::memory_order_acquire), high_quality_profile);
    active_high_quality_ = high_quality_profile;

    // ---------------------------------------------------------------
    // Initialize CaptureEngine
    // ---------------------------------------------------------------
    if (!capture_->initialize(probe_.d3d_device.Get(),
                               probe_.d3d_context.Get(),
                               frame_queue_.get(),
                               {enc_prof.width, enc_prof.height},
                               enc_prof.fps))
    {
        notify_error(L"Capture engine initialization failed");
        rollback_startup(L"capture_engine_initialization_failed");
        return false;
    }
    capture_->set_sync_anchor_100ns(sync_.anchor_100ns());

    // ---------------------------------------------------------------
    // Initialize AudioEngine (microphone)
    // ---------------------------------------------------------------
    if (!audio_->initialize(audio_queue_.get(), AudioCaptureMode::Microphone)) {
        notify_error(L"Microphone audio init failed (no microphone?)");
        SR_LOG_WARN(L"Continuing without microphone audio");
    } else {
        audio_->set_sync_anchor_100ns(sync_.anchor_100ns());
    }

    // ---------------------------------------------------------------
    // Initialize Loopback AudioEngine (system/desktop audio)
    // ---------------------------------------------------------------
    if (!loopback_audio_->initialize(loopback_queue_.get(), AudioCaptureMode::Loopback)) {
        notify_error(L"System audio loopback init failed");
        SR_LOG_WARN(L"Continuing without system audio capture");
    } else {
        loopback_audio_->set_sync_anchor_100ns(sync_.anchor_100ns());
    }

    // ---------------------------------------------------------------
    // Initialize VideoEncoder with the actual capture output dimensions.
    // ---------------------------------------------------------------
    enc_prof.width  = capture_->width()  ? capture_->width()  : 1920;
    enc_prof.height = capture_->height() ? capture_->height() : 1080;

    if (!encoder_->initialize(enc_prof,
                               probe_.dxgi_device_manager.Get(),
                               probe_.d3d_device.Get(),
                               probe_.d3d_context.Get()))
    {
        notify_error(L"Video encoder initialization failed");
        rollback_startup(L"video_encoder_initialization_failed");
        return false;
    }

    // ---------------------------------------------------------------
    // Initialize MuxWriter
    // ---------------------------------------------------------------
    MuxConfig mux_cfg;
    mux_cfg.video_width  = encoder_->output_width();
    mux_cfg.video_height = encoder_->output_height();
    mux_cfg.video_fps_num= encoder_->output_fps();
    // Both inputs are aligned/mixed into one canonical 48 kHz float stereo stream.
    mux_cfg.audio_sample_rate      = 48'000;
    mux_cfg.audio_channels         = 2;
    mux_cfg.audio_bits_per_sample  = 32;
    mux_cfg.audio_is_float         = true;

    if (!muxer_->initialize(current_partial_path_, current_output_path_, mux_cfg)) {
        notify_error(L"Mux writer initialization failed");
        rollback_startup(L"mux_writer_initialization_failed");
        return false;
    }

    SessionDiagnostics::StartInfo diagnostics_start;
    diagnostics_start.build_commit = SR_BUILD_COMMIT;
    diagnostics_start.output_path = current_output_path_;
    diagnostics_start.adapter_name = probe_.adapter_name;
    diagnostics_start.probed_encoder_name =
        probe_.hw_encoder_available ? probe_.encoder_name : L"not available";
    diagnostics_start.encoder_mode = encoder_mode_label(encoder_->mode());
    diagnostics_start.power_state = last_power_ac_.load(std::memory_order_acquire) ? L"AC" : L"Battery";
    diagnostics_start.high_quality = high_quality_profile;
    diagnostics_start.width = encoder_->output_width();
    diagnostics_start.height = encoder_->output_height();
    diagnostics_start.fps = encoder_->output_fps();
    diagnostics_start.bitrate_bps = encoder_->output_bitrate();
    diagnostics_.write_start(diagnostics_start);

    // ---------------------------------------------------------------
    // Start engines
    // ---------------------------------------------------------------
    frames_encoded_.store(0, std::memory_order_relaxed);
    video_write_failures_.store(0, std::memory_order_relaxed);
    audio_write_failures_.store(0, std::memory_order_relaxed);
    audio_source_start_failures_.store(0, std::memory_order_relaxed);
    audio_mixer_stats_ = {};
    output_failure_reported_.store(false, std::memory_order_relaxed);
    stopping_from_pause_.store(false, std::memory_order_relaxed);
    audio_written_.store(0,  std::memory_order_relaxed);
    gpu_memory_sample_available_.store(false, std::memory_order_relaxed);
    gpu_memory_sample_tick_ms_.store(0, std::memory_order_relaxed);
    gpu_local_usage_bytes_.store(0, std::memory_order_relaxed);
    gpu_local_budget_bytes_.store(0, std::memory_order_relaxed);
    gpu_nonlocal_usage_bytes_.store(0, std::memory_order_relaxed);
    gpu_nonlocal_budget_bytes_.store(0, std::memory_order_relaxed);
    video_encode_timing_.reset();
    video_mux_timing_.reset();
    audio_mux_timing_.reset();
    capture_->reset_counters();
    frame_queue_->reset_high_watermark();
    audio_queue_->reset_high_watermark();
    loopback_queue_->reset_high_watermark();
    telemetry_.reset(); // T037: clear counters for new session

    // T038: initialise frame pacer for this session's fps
    pacer_.initialize(enc_prof.fps);

    // T039: register device-lost callback — fires when D3D device is removed
    capture_->set_device_lost_callback([this]() {
        SR_LOG_ERROR(L"[T039] Device-lost event received — auto-stopping recording");
        diagnostics_.write_event(L"event=device_lost stop_requested=1");
        notify_error(L"\u26A0 Graphics device was reset or removed. Recording stopped.");
        // Do not tear down WGC from inside its FrameArrived callback. Ask the
        // UI thread to start the normal stop worker after this callback exits.
        if (on_stop_requested_) {
            on_stop_requested_();
        } else {
            SR_LOG_ERROR(L"No stop-request dispatcher is registered; WGC teardown deferred");
            diagnostics_.write_event(L"event=device_lost stop_dispatcher=missing");
        }
    });

    encode_running_.store(true, std::memory_order_release);
    try {
        encode_thread_ = std::thread(&SessionController::encode_loop, this);
    } catch (const std::system_error& e) {
        encode_running_.store(false, std::memory_order_release);
        SR_LOG_ERROR(L"Could not create recording pipeline thread: %S", e.what());
        rollback_startup(L"encode_thread_creation_failed");
        return false;
    }

    if (!capture_->start()) {
        diagnostics_.write_event(L"event=start_stage stage=capture state=failed");
        notify_error(L"Capture start failed");
        rollback_startup(L"capture_start_failed");
        return false;
    }
    diagnostics_.write_event(L"event=start_stage stage=capture state=complete");
    const bool microphone_started = audio_->start();
    const bool loopback_started = loopback_audio_->start();
    audio_source_start_failures_.store(
        static_cast<uint32_t>(!microphone_started) + static_cast<uint32_t>(!loopback_started),
        std::memory_order_relaxed);
    diagnostics_.write_event(microphone_started
        ? L"event=start_stage stage=microphone state=complete"
        : L"event=start_stage stage=microphone state=unavailable");
    diagnostics_.write_event(loopback_started
        ? L"event=start_stage stage=loopback state=complete"
        : L"event=start_stage stage=loopback state=unavailable");

    try {
        // T031: Start async disk space polling — auto-stop on < 500 MB free
        if (storage_) {
            storage_->startDiskSpacePolling([this]() {
                if (!machine_.is_idle()) {
                    SR_LOG_WARN(L"Auto-stopping: disk space critically low");
                    notify_error(L"\u26A0 Disk space critically low! Recording auto-stopped.");
                    stop();
                }
            });
        }
        start_diagnostics_monitor();
    } catch (const std::system_error& e) {
        SR_LOG_ERROR(L"Could not create recording monitor thread: %S", e.what());
        rollback_startup(L"recording_monitor_thread_creation_failed");
        return false;
    }
    diagnostics_.write_event(L"event=recording_started");
    notify_status(L"Recording...");
    SR_LOG_INFO(L"Recording started -> %s", current_output_path_.c_str());
    return true;
}

bool SessionController::stop() {
    const bool was_paused = machine_.is_paused();
    if (was_paused) stopping_from_pause_.store(true, std::memory_order_release);
    bool was_recording = machine_.is_recording() || machine_.is_paused();

    if (!machine_.transition(SessionEvent::Stop)) {
        stopping_from_pause_.store(false, std::memory_order_release);
        return false;
    }
    if (was_paused) {
        // Keep the session clock frozen while producers stop and queued media
        // drains. This prevents a brief post-stop resume window from admitting
        // frames or audio captured while the user had paused the recording.
        diagnostics_.write_event(L"event=stop_requested_while_paused pause_clock_frozen=1");
    }
    notify_status(L"Stopping...");
    diagnostics_.write_event(L"event=stop_requested");

    // T031: Stop disk space polling (handles re-entrant call from poll thread)
    diagnostics_stage_.store(DiagnosticsStage::StoppingDiskMonitor, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=stop_disk_monitor state=begin");
    if (storage_) storage_->stopDiskSpacePolling();
    diagnostics_.write_event(L"event=stop_stage stage=stop_disk_monitor state=complete");

    // Stop producers first
    diagnostics_stage_.store(DiagnosticsStage::StoppingCapture, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=stop_capture state=begin");
    capture_->stop();
    diagnostics_.write_event(L"event=stop_stage stage=stop_capture state=complete");
    diagnostics_stage_.store(DiagnosticsStage::StoppingMicrophone, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=stop_microphone state=begin");
    audio_->stop();
    diagnostics_.write_event(L"event=stop_stage stage=stop_microphone state=complete");
    diagnostics_stage_.store(DiagnosticsStage::StoppingLoopback, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=stop_loopback state=begin");
    loopback_audio_->stop();
    diagnostics_.write_event(L"event=stop_stage stage=stop_loopback state=complete");

    // Stop encode loop (it drains remaining frames)
    diagnostics_stage_.store(DiagnosticsStage::JoiningEncodeThread, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=join_encode_thread state=begin");
    encode_running_.store(false, std::memory_order_release);
    if (encode_thread_.joinable()) encode_thread_.join();
    diagnostics_.write_event(L"event=stop_stage stage=join_encode_thread state=complete");

    // Flush encoder and write remaining samples
    bool encoder_drain_ok = true;
    uint64_t encoder_drain_ms = 0;
    uint64_t encoder_pending_inputs = 0;
    VideoEncoder::DiagnosticsSnapshot encoder_final_state{};
    size_t encoder_drain_samples = 0;
    if (was_recording) {
        diagnostics_stage_.store(DiagnosticsStage::FlushingEncoder, std::memory_order_relaxed);
        diagnostics_.write_event(L"event=stop_stage stage=encoder_flush state=begin");
        std::vector<VideoEncoder::AsyncEncodedOutput> leftover;
        const auto drain_started = std::chrono::steady_clock::now();
        encoder_drain_ok = encoder_->flush(leftover);
        encoder_drain_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - drain_started).count());
        encoder_final_state = encoder_->diagnostics_snapshot();
        encoder_pending_inputs = encoder_final_state.pending_inputs;
        encoder_drain_samples = leftover.size();
        const auto event_wait = encoder_final_state.async_event_wait_us;
        const auto queue_residence = encoder_final_state.async_input_queue_residence_us;
        const auto process_input = encoder_final_state.async_process_input_us;
        const auto process_output = encoder_final_state.async_process_output_us;
        const auto output_latency = encoder_final_state.async_accept_to_output_us;
        diagnostics_.write_event(
            L"event=encoder_async_final need_input_events=" +
            std::to_wstring(encoder_final_state.async_need_input_events) +
            L" have_output_events=" + std::to_wstring(encoder_final_state.async_have_output_events) +
            L" worker_wakeups=" + std::to_wstring(encoder_final_state.async_worker_wakeups) +
            L" input_queue_high_watermark=" +
            std::to_wstring(encoder_final_state.deferred_inputs_high_watermark) +
            L" output_queue_high_watermark=" +
            std::to_wstring(encoder_final_state.async_output_queue_high_watermark) +
            L" output_buffer_capacity_high_watermark=" +
            std::to_wstring(encoder_final_state.async_output_buffer_capacity_high_watermark) +
            L" input_credit_high_watermark=" +
            std::to_wstring(encoder_final_state.async_input_credit_high_watermark) +
            L" event_wait_p95_us=" + std::to_wstring(event_wait.p95_upper_us) +
            L" queue_residence_p95_us=" + std::to_wstring(queue_residence.p95_upper_us) +
            L" process_input_p95_us=" + std::to_wstring(process_input.p95_upper_us) +
            L" process_output_p95_us=" + std::to_wstring(process_output.p95_upper_us) +
            L" accepted_to_output_p95_us=" + std::to_wstring(output_latency.p95_upper_us));
        diagnostics_.write_event(L"event=stop_stage stage=encoder_flush state=" +
                                 std::wstring(encoder_drain_ok ? L"complete" : L"incomplete") +
                                 L" samples=" + std::to_wstring(leftover.size()) +
                                 L" pending_inputs=" + std::to_wstring(encoder_pending_inputs) +
                                 L" elapsed_ms=" + std::to_wstring(encoder_drain_ms));
        SR_LOG_INFO(L"Encoder flush produced %zu samples", leftover.size());
        for (auto& output : leftover) {
            const auto mux_write_started = std::chrono::steady_clock::now();
            if (muxer_->write_video(output.sample.Get())) {
                frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                telemetry_.on_frame_encoded();
                if (output.synthetic) telemetry_.on_duplicate_inserted();
            } else {
                const uint32_t failures = video_write_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failures == 1 || (failures % 120) == 0) {
                    SR_LOG_ERROR(L"Muxer rejected drained encoded video sample (frame_id=%llu total failures=%u)",
                                 static_cast<unsigned long long>(output.frame_id), failures);
                }
                diagnostics_.write_event(L"event=encoded_video_write_failed frame_id=" +
                                         std::to_wstring(output.frame_id) +
                                         L" synthetic=" + std::to_wstring(output.synthetic ? 1 : 0) +
                                         L" drain=1");
                report_recording_loss(L"video_mux");
            }
            const auto mux_write_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - mux_write_started).count();
            video_mux_timing_.record(static_cast<uint64_t>((std::max)(0LL, mux_write_us)));
        }
        encoder_->shutdown();
        encoder_final_state = encoder_->diagnostics_snapshot();
        encoder_pending_inputs = encoder_final_state.pending_inputs;
        const auto drain_mux_timing = video_mux_timing_.snapshot();
        diagnostics_.write_event(L"event=encoder_drain_enqueue_timing samples=" +
            std::to_wstring(drain_mux_timing.samples) + L" total_us=" +
            std::to_wstring(drain_mux_timing.total_us) + L" max_us=" +
            std::to_wstring(drain_mux_timing.max_us));
    }

    // Finalize mux (rename .partial.mp4 -> .mp4)
    diagnostics_stage_.store(DiagnosticsStage::FinalizingMp4, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=finalize_mp4 state=begin");
    const auto finalize_started = std::chrono::steady_clock::now();
    const bool finalized = muxer_->finalize();
    const uint64_t finalize_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - finalize_started).count());
    diagnostics_.write_event(finalized
        ? L"event=stop_stage stage=finalize_mp4 state=complete"
        : L"event=stop_stage stage=finalize_mp4 state=failed");
    if (!finalized) {
        notify_error(L"Failed to finalize recording file. Partial file kept.");
    }
    const auto writer_final = muxer_->snapshot();
    diagnostics_.write_event(
        L"event=writer_final accepted=" + std::to_wstring(writer_final.samples_enqueued) +
        L" written=" + std::to_wstring(writer_final.samples_written) +
        L" video_accepted=" + std::to_wstring(writer_final.video_samples_enqueued) +
        L" video_written=" + std::to_wstring(writer_final.video_samples_written) +
        L" audio_accepted=" + std::to_wstring(writer_final.audio_samples_enqueued) +
        L" audio_written=" + std::to_wstring(writer_final.audio_samples_written) +
        L" queue_high_packets=" + std::to_wstring(writer_final.queue.packet_high_watermark) +
        L" queue_high_bytes=" + std::to_wstring(writer_final.queue.queued_bytes_high_watermark) +
        L" queue_max_stream_span_100ns=" +
        std::to_wstring(writer_final.queue.max_stream_span_100ns) +
        L" rejected=" + std::to_wstring(writer_final.queue.rejected_packets) +
        L" discarded=" + std::to_wstring(writer_final.queue.discarded_packets) +
        L" write_failures=" + std::to_wstring(writer_final.write_failures) +
        L" invalid_samples=" + std::to_wstring(writer_final.invalid_sample_rejections) +
        L" write_total_us=" + std::to_wstring(writer_final.write_total_us) +
        L" write_max_us=" + std::to_wstring(writer_final.write_max_us) +
        L" writer_drain_ms=" + std::to_wstring(writer_final.writer_drain_ms) +
        L" sink_finalize_ms=" + std::to_wstring(writer_final.sink_finalize_ms) +
        L" write_p50_us=" + std::to_wstring(writer_final.write_latency.p50_upper_us) +
        L" write_p95_us=" + std::to_wstring(writer_final.write_latency.p95_upper_us) +
        L" write_p99_us=" + std::to_wstring(writer_final.write_latency.p99_upper_us) +
        L" bytes_written=" + std::to_wstring(writer_final.bytes_written) +
        L" last_error_hr=" + std::to_wstring(static_cast<uint32_t>(writer_final.last_error)));

    // Stop/join diagnostics before publishing Idle; a new session must not
    // overlap the prior session's sampler or persistent log path.
    diagnostics_stage_.store(DiagnosticsStage::Complete, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_complete");
    stop_diagnostics_monitor();

    SessionDiagnostics::StopInfo diagnostics_stop;
    diagnostics_stop.encoder_mode = encoder_mode_label(encoder_->mode());
    diagnostics_stop.frames_captured = capture_->frames_captured();
    diagnostics_stop.frames_encoded = static_cast<uint32_t>((std::min<uint64_t>)(
        writer_final.video_samples_written, (std::numeric_limits<uint32_t>::max)()));
    diagnostics_stop.frames_dropped = capture_->frames_dropped();
    diagnostics_stop.capture_resize_failures = capture_->resize_failures();
    diagnostics_stop.capture_texture_acquisition_failures = capture_->texture_acquisition_failures();
    diagnostics_stop.queue_drops = capture_->queue_drops();
    const uint64_t video_not_written =
        writer_final.video_samples_enqueued > writer_final.video_samples_written
            ? writer_final.video_samples_enqueued - writer_final.video_samples_written : 0;
    const auto saturating_failure_count = [](uint32_t base, uint64_t additional) {
        return static_cast<uint32_t>((std::min<uint64_t>)(
            static_cast<uint64_t>(base) + additional,
            (std::numeric_limits<uint32_t>::max)()));
    };
    diagnostics_stop.video_write_failures =
        saturating_failure_count(video_write_failures_.load(), video_not_written);
    diagnostics_stop.audio_queue_drops = audio_->queue_drops() + loopback_audio_->queue_drops();
    diagnostics_stop.audio_resampler_failures =
        audio_->resampler_failures() + loopback_audio_->resampler_failures();
    const uint64_t audio_not_written =
        writer_final.audio_samples_enqueued > writer_final.audio_samples_written
            ? writer_final.audio_samples_enqueued - writer_final.audio_samples_written : 0;
    diagnostics_stop.audio_write_failures =
        saturating_failure_count(audio_write_failures_.load(), audio_not_written);
    diagnostics_stop.audio_source_start_failures = audio_source_start_failures_.load();
    diagnostics_stop.audio_mixer_dropped_packets = audio_mixer_stats_.dropped_packets;
    diagnostics_stop.audio_mixer_rejected_packets = audio_mixer_stats_.rejected_packets;
    diagnostics_stop.audio_mixer_high_water_packets = (std::max)(
        audio_mixer_stats_.microphone_high_water_packets,
        audio_mixer_stats_.loopback_high_water_packets);
    diagnostics_stop.audio_mixer_high_water_bytes = (std::max)(
        audio_mixer_stats_.microphone_high_water_bytes,
        audio_mixer_stats_.loopback_high_water_bytes);
    diagnostics_stop.audio_mixer_high_water_duration_100ns = (std::max)(
        audio_mixer_stats_.microphone_high_water_duration_100ns,
        audio_mixer_stats_.loopback_high_water_duration_100ns);
    diagnostics_stop.audio_packets = static_cast<uint32_t>((std::min<uint64_t>)(
        writer_final.audio_samples_written, (std::numeric_limits<uint32_t>::max)()));
    diagnostics_stop.encoder_drain_complete = encoder_drain_ok;
    diagnostics_stop.encoder_pending_inputs = encoder_pending_inputs;
    diagnostics_stop.encoder_deferred_inputs_pending = encoder_final_state.deferred_inputs_pending;
    diagnostics_stop.encoder_deferred_inputs_high_watermark = encoder_final_state.deferred_inputs_high_watermark;
    diagnostics_stop.encoder_inputs_offered = encoder_final_state.input_samples_offered;
    diagnostics_stop.encoder_inputs_accepted = encoder_final_state.input_samples_accepted;
    diagnostics_stop.encoder_outputs_produced = encoder_final_state.output_samples_produced;
    diagnostics_stop.encoder_inputs_not_accepted = encoder_final_state.input_samples_not_accepted;
    diagnostics_stop.encoder_input_errors = encoder_final_state.input_errors;
    diagnostics_stop.encoder_inputs_deferred = encoder_final_state.input_samples_deferred_without_event;
    diagnostics_stop.encoder_inputs_abandoned = encoder_final_state.pending_inputs_abandoned;
    diagnostics_stop.encoder_inputs_retried = encoder_final_state.input_samples_retried;
    diagnostics_stop.encoder_timestamp_errors = encoder_final_state.output_timestamp_errors;
    diagnostics_stop.encoder_input_preparation_errors = encoder_final_state.input_preparation_errors;
    diagnostics_stop.encoder_output_errors = encoder_final_state.output_errors;
    diagnostics_stop.encoder_scheduler_errors = encoder_final_state.scheduler_errors;
    diagnostics_stop.encoder_input_accounting_errors = encoder_final_state.input_accounting_errors;
    diagnostics_stop.encoder_drain_samples = encoder_drain_samples;
    diagnostics_stop.encoder_drain_ms = encoder_drain_ms;
    diagnostics_stop.finalize_ms = finalize_ms;

    // Drop idle capture, audio endpoint and encoder state after their final
    // counters have been captured. A later session initializes them afresh.
    audio_->shutdown();
    loopback_audio_->shutdown();

    // Wait for queued session events so logger failures are reflected in the
    // outcome. The diagnostic writer itself is bounded and this wait is bounded.
    if (!flush_persistent_log(std::chrono::seconds(2))) {
        diagnostics_stop.diagnostic_log_dropped = 1;
    } else {
        const uint64_t dropped_now = persistent_log_dropped_count();
        diagnostics_stop.diagnostic_log_dropped = dropped_now >= diagnostic_log_dropped_at_start_
            ? dropped_now - diagnostic_log_dropped_at_start_ : dropped_now;
    }

    RecordingOutcomeInputs outcome_input;
    outcome_input.finalized = finalized;
    outcome_input.encoder_drain_complete = encoder_drain_ok;
    outcome_input.frame_drops = diagnostics_stop.frames_dropped;
    outcome_input.capture_resize_failures = diagnostics_stop.capture_resize_failures;
    outcome_input.capture_texture_acquisition_failures = diagnostics_stop.capture_texture_acquisition_failures;
    outcome_input.video_write_failures = diagnostics_stop.video_write_failures;
    outcome_input.audio_queue_drops = diagnostics_stop.audio_queue_drops;
    outcome_input.audio_resampler_failures = diagnostics_stop.audio_resampler_failures;
    outcome_input.audio_write_failures = diagnostics_stop.audio_write_failures;
    outcome_input.audio_source_start_failures = diagnostics_stop.audio_source_start_failures;
    outcome_input.audio_mixer_dropped_packets = diagnostics_stop.audio_mixer_dropped_packets;
    outcome_input.audio_mixer_rejected_packets = diagnostics_stop.audio_mixer_rejected_packets;
    outcome_input.encoder_inputs_not_accepted = diagnostics_stop.encoder_inputs_not_accepted;
    outcome_input.encoder_input_errors = diagnostics_stop.encoder_input_errors;
    outcome_input.encoder_inputs_deferred = diagnostics_stop.encoder_inputs_deferred;
    outcome_input.encoder_inputs_abandoned = diagnostics_stop.encoder_inputs_abandoned;
    outcome_input.encoder_timestamp_errors = diagnostics_stop.encoder_timestamp_errors;
    outcome_input.encoder_input_preparation_errors = diagnostics_stop.encoder_input_preparation_errors;
    outcome_input.encoder_output_errors = diagnostics_stop.encoder_output_errors;
    outcome_input.encoder_scheduler_errors = diagnostics_stop.encoder_scheduler_errors;
    outcome_input.encoder_input_accounting_errors = diagnostics_stop.encoder_input_accounting_errors;
    outcome_input.encoder_pending_inputs = diagnostics_stop.encoder_pending_inputs;
    outcome_input.warnings = diagnostics_stop.diagnostic_log_dropped +
        pipeline_stall_events_.load(std::memory_order_relaxed);
    const RecordingOutcome outcome = classify_recording_outcome(outcome_input);
    last_recording_outcome_.store(outcome, std::memory_order_release);
    diagnostics_stop.status = recording_outcome_name(outcome);
    if (outcome == RecordingOutcome::Incomplete) {
        notify_error(L"The MP4 was finalized, but diagnostics found possible missing or invalid media. Review its diagnostics file before relying on it.");
    }

    diagnostics_.write_stop(diagnostics_stop);
    SR_LOG_INFO(L"Recording stopped. Encoded: %u frames, audio pkts: %u",
                frames_encoded_.load(), audio_written_.load());
    if (!flush_persistent_log(std::chrono::seconds(2))) {
        OutputDebugStringW(L"[SR][WARN] Persistent diagnostics did not flush within 2 seconds.\n");
    }

    clear_persistent_log_path();
    machine_.transition(SessionEvent::Finalized);
    stopping_from_pause_.store(false, std::memory_order_release);
    notify_status(L"Idle");
    return true;
}

void SessionController::start_diagnostics_monitor() {
    diagnostics_started_ = std::chrono::steady_clock::now();
    diagnostics_monitor_running_.store(true, std::memory_order_release);
    diagnostics_thread_ = std::thread(&SessionController::diagnostics_monitor_loop, this);
}

void SessionController::stop_diagnostics_monitor() {
    diagnostics_monitor_running_.store(false, std::memory_order_release);
    diagnostics_wait_cv_.notify_all();
    if (diagnostics_thread_.joinable() &&
        diagnostics_thread_.get_id() != std::this_thread::get_id()) {
        diagnostics_thread_.join();
    }
}

void SessionController::diagnostics_monitor_loop() {
    std::unique_lock<std::mutex> wait_lock(diagnostics_wait_mutex_);
    constexpr auto kMonitorInterval = std::chrono::seconds(5);
    constexpr auto kHeartbeatInterval = std::chrono::seconds(30);
    auto last_heartbeat = diagnostics_started_;
    bool write_stall_reported = false;
    uint32_t last_encoded = 0;
    uint32_t last_audio_drops = 0;
    ULONGLONG last_video_progress_ms = GetTickCount64();
    const ULONGLONG diagnostics_start_ms = last_video_progress_ms;
    ULONGLONG last_resource_sample_ms = last_video_progress_ms;
    uint64_t last_process_cpu_100ns = 0;
    const bool have_initial_cpu_sample = process_cpu_time_100ns(last_process_cpu_100ns);
    bool have_previous_cpu_sample = have_initial_cpu_sample;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint32_t logical_processors = (std::max)(1u,
        static_cast<uint32_t>(system_info.dwNumberOfProcessors));
    uint32_t last_captured_frames = 0;
    uint32_t last_written_frames = 0;
    uint32_t last_duplicate_frames = 0;
    uint32_t last_dropped_frames = 0;
    uint32_t last_capture_queue_drops = 0;

    ComPtr<IDXGIAdapter3> perf_adapter3;
    if (probe_.d3d_device) {
        ComPtr<IDXGIDevice> dxgi_device;
        if (SUCCEEDED(probe_.d3d_device.As(&dxgi_device)) && dxgi_device) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
                adapter.As(&perf_adapter3);
            }
        }
    }

    while (diagnostics_monitor_running_.load(std::memory_order_acquire)) {
        if (diagnostics_wait_cv_.wait_for(wait_lock, kMonitorInterval, [this]() {
                return !diagnostics_monitor_running_.load(std::memory_order_acquire);
            })) {
            break;
        }

        wait_lock.unlock();
        const uint64_t write_started = muxer_ ? muxer_->active_write_started_ms() : 0;
        const uint64_t now_ms = GetTickCount64();
        if (perf_adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal_info{};
            const HRESULT local_hr = perf_adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info);
            const HRESULT nonlocal_hr = perf_adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal_info);
            if (SUCCEEDED(local_hr) && SUCCEEDED(nonlocal_hr)) {
                gpu_local_usage_bytes_.store(local_info.CurrentUsage, std::memory_order_relaxed);
                gpu_local_budget_bytes_.store(local_info.Budget, std::memory_order_relaxed);
                gpu_nonlocal_usage_bytes_.store(nonlocal_info.CurrentUsage, std::memory_order_relaxed);
                gpu_nonlocal_budget_bytes_.store(nonlocal_info.Budget, std::memory_order_relaxed);
                gpu_memory_sample_tick_ms_.store(now_ms, std::memory_order_relaxed);
                gpu_memory_sample_available_.store(true, std::memory_order_relaxed);
            }
        }
        const uint64_t resource_interval_ms = now_ms >= last_resource_sample_ms
            ? now_ms - last_resource_sample_ms : 0;
        uint64_t process_cpu_pct_milli = 0;
        bool process_cpu_sample_available = false;
        uint64_t current_process_cpu_100ns = 0;
        if (process_cpu_time_100ns(current_process_cpu_100ns)) {
            const uint64_t elapsed_ms = now_ms - last_resource_sample_ms;
            if (have_previous_cpu_sample && elapsed_ms > 0 &&
                current_process_cpu_100ns >= last_process_cpu_100ns) {
                const uint64_t cpu_delta_100ns = current_process_cpu_100ns - last_process_cpu_100ns;
                const double denominator = static_cast<double>(elapsed_ms) * 10'000.0 * logical_processors;
                process_cpu_pct_milli = static_cast<uint64_t>(
                    (static_cast<double>(cpu_delta_100ns) * 100'000.0 / denominator) + 0.5);
                process_cpu_sample_available = true;
            }
            last_process_cpu_100ns = current_process_cpu_100ns;
            have_previous_cpu_sample = true;
        } else {
            have_previous_cpu_sample = false;
        }
        last_resource_sample_ms = now_ms;

        PROCESS_MEMORY_COUNTERS_EX process_memory{};
        process_memory.cb = sizeof(process_memory);
        const bool process_memory_available = GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process_memory),
            sizeof(process_memory)) != FALSE;
        MEMORYSTATUSEX system_memory{};
        system_memory.dwLength = sizeof(system_memory);
        const bool system_memory_available = GlobalMemoryStatusEx(&system_memory) != FALSE;
        SYSTEM_POWER_STATUS power_status{};
        const bool power_status_available = GetSystemPowerStatus(&power_status) != FALSE;

        const uint64_t mb = 1024ull * 1024ull;
        const bool gpu_memory_available = gpu_memory_sample_available_.load(std::memory_order_relaxed);
        const uint64_t gpu_local_usage = gpu_local_usage_bytes_.load(std::memory_order_relaxed);
        const uint64_t gpu_local_budget = gpu_local_budget_bytes_.load(std::memory_order_relaxed);
        const uint64_t gpu_nonlocal_usage = gpu_nonlocal_usage_bytes_.load(std::memory_order_relaxed);
        const uint64_t gpu_nonlocal_budget = gpu_nonlocal_budget_bytes_.load(std::memory_order_relaxed);
        const uint64_t gpu_sample_tick_ms = gpu_memory_sample_tick_ms_.load(std::memory_order_relaxed);
        const uint64_t gpu_sample_age_ms = gpu_memory_available && now_ms >= gpu_sample_tick_ms
            ? now_ms - gpu_sample_tick_ms : 0;
        const std::wstring gpu_fields = format_dxgi_memory_fields(
            gpu_memory_available, gpu_sample_age_ms, gpu_local_usage, gpu_local_budget,
            gpu_nonlocal_usage, gpu_nonlocal_budget);
        const uint32_t captured_frames = capture_ ? capture_->frames_captured() : 0;
        const auto writer_live = muxer_ ? muxer_->snapshot() : MuxWriter::StatsSnapshot{};
        const uint32_t written_frames = static_cast<uint32_t>((std::min<uint64_t>)(
            writer_live.video_samples_written, (std::numeric_limits<uint32_t>::max)()));
        const uint32_t dropped_frames = capture_ ? capture_->frames_dropped() : 0;
        const uint32_t capture_queue_drops = capture_ ? capture_->queue_drops() : 0;
        const uint32_t cadence_rejections = capture_ ? capture_->cadence_rejections() : 0;
        const uint64_t conversion_samples = capture_ ? capture_->conversion_submission_samples() : 0;
        const uint64_t conversion_total_us = capture_ ? capture_->conversion_submission_total_us() : 0;
        const uint64_t conversion_max_us = capture_ ? capture_->conversion_submission_max_us() : 0;
        const auto conversion_latency = capture_ ? capture_->conversion_submission_latency()
                                                 : LatencyHistogram::Snapshot{};
        const size_t video_queue_size = frame_queue_ ? frame_queue_->size() : 0;
        const size_t video_queue_high = frame_queue_ ? frame_queue_->high_watermark() : 0;
        const size_t mic_queue_size = audio_queue_ ? audio_queue_->size() : 0;
        const size_t mic_queue_high = audio_queue_ ? audio_queue_->high_watermark() : 0;
        const size_t loopback_queue_size = loopback_queue_ ? loopback_queue_->size() : 0;
        const size_t loopback_queue_high = loopback_queue_ ? loopback_queue_->high_watermark() : 0;
        const auto live_telemetry = telemetry_.snapshot(0, false);
        const uint32_t capture_fps_milli = resource_interval_ms > 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(captured_frames - last_captured_frames) * 1'000'000ull) / resource_interval_ms)
            : 0;
        const uint32_t written_fps_milli = resource_interval_ms > 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(written_frames - last_written_frames) * 1'000'000ull) / resource_interval_ms)
            : 0;
        const uint32_t capture_drop_delta = dropped_frames - last_dropped_frames;
        const uint32_t capture_queue_drop_delta = capture_queue_drops - last_capture_queue_drops;
        const uint32_t duplicate_video_delta = live_telemetry.dup_frames - last_duplicate_frames;
        const auto encoder_metrics = encoder_ ? encoder_->diagnostics_snapshot()
                                              : VideoEncoder::DiagnosticsSnapshot{};
        const auto encode_timing = video_encode_timing_.snapshot();
        const auto video_mux_timing = video_mux_timing_.snapshot();
        const auto audio_mux_timing = audio_mux_timing_.snapshot();
        wchar_t resource_event[4096]{};
        _snwprintf_s(resource_event, _countof(resource_event), _TRUNCATE,
            L"event=resource_sample elapsed_s=%llu "
            L"process_cpu_available=%u process_cpu_pct_milli=%llu cpu_scope=process_normalized_to_logical_processors "
            L"process_memory_available=%u process_private_mb=%llu process_working_set_mb=%llu "
            L"system_memory_available=%u system_memory_load_pct=%lu system_physical_total_mb=%llu system_physical_available_mb=%llu "
            L"power_status_available=%u power_source=%s battery_percent=%d battery_state=%s "
            L"%s "
            L"capture_frames=%u capture_fps_milli=%u capture_drop_delta=%u capture_queue_drop_delta=%u "
            L"capture_cadence_rejections=%u conversion_submit_samples=%llu conversion_submit_avg_us=%llu conversion_submit_max_us=%llu conversion_submit_p50_us=%llu conversion_submit_p95_us=%llu conversion_submit_p99_us=%llu "
            L"video_queue_depth=%llu video_queue_high_watermark=%llu mic_queue_depth=%llu mic_queue_high_watermark=%llu "
            L"loopback_queue_depth=%llu loopback_queue_high_watermark=%llu "
            L"video_samples_written=%u video_write_fps_milli=%u duplicate_video_delta=%u "
            L"mft_input_offered=%llu mft_input_accepted=%llu mft_input_not_accepted=%llu "
            L"mft_input_deferred_no_event=%llu mft_input_errors=%llu mft_output_samples=%llu mft_output_waits=%llu "
            L"mft_input_pending=%llu mft_input_pending_high_watermark=%llu "
            L"mft_deferred_pending=%llu mft_deferred_high_watermark=%llu "
            L"mft_input_retried=%llu mft_output_timestamp_errors=%llu "
            L"encode_timing_samples=%llu encode_avg_us=%llu encode_max_us=%llu encode_p50_us=%llu encode_p95_us=%llu encode_p99_us=%llu "
            L"video_enqueue_samples=%llu video_enqueue_avg_us=%llu video_enqueue_max_us=%llu video_enqueue_p50_us=%llu video_enqueue_p95_us=%llu video_enqueue_p99_us=%llu "
            L"audio_enqueue_samples=%llu audio_enqueue_avg_us=%llu audio_enqueue_max_us=%llu audio_enqueue_p50_us=%llu audio_enqueue_p95_us=%llu audio_enqueue_p99_us=%llu "
            L"diagnostic_log_dropped=%llu",
            static_cast<unsigned long long>((now_ms - diagnostics_start_ms) / 1000),
            process_cpu_sample_available ? 1u : 0u,
            static_cast<unsigned long long>(process_cpu_pct_milli),
            process_memory_available ? 1u : 0u,
            static_cast<unsigned long long>(process_memory_available ? process_memory.PrivateUsage / mb : 0),
            static_cast<unsigned long long>(process_memory_available ? process_memory.WorkingSetSize / mb : 0),
            system_memory_available ? 1u : 0u,
            system_memory_available ? system_memory.dwMemoryLoad : 0,
            static_cast<unsigned long long>(system_memory_available ? system_memory.ullTotalPhys / mb : 0),
            static_cast<unsigned long long>(system_memory_available ? system_memory.ullAvailPhys / mb : 0),
            power_status_available ? 1u : 0u,
            power_status_available ? power_source_label(power_status.ACLineStatus) : L"unknown",
            power_status_available && power_status.BatteryLifePercent != 255
                ? static_cast<int>(power_status.BatteryLifePercent) : -1,
            power_status_available ? battery_state_label(power_status.BatteryFlag) : L"unknown",
            gpu_fields.c_str(),
            captured_frames, capture_fps_milli, capture_drop_delta, capture_queue_drop_delta,
            cadence_rejections,
            static_cast<unsigned long long>(conversion_samples),
            static_cast<unsigned long long>(conversion_samples ? conversion_total_us / conversion_samples : 0),
            static_cast<unsigned long long>(conversion_max_us),
            static_cast<unsigned long long>(conversion_latency.p50_upper_us),
            static_cast<unsigned long long>(conversion_latency.p95_upper_us),
            static_cast<unsigned long long>(conversion_latency.p99_upper_us),
            static_cast<unsigned long long>(video_queue_size),
            static_cast<unsigned long long>(video_queue_high),
            static_cast<unsigned long long>(mic_queue_size),
            static_cast<unsigned long long>(mic_queue_high),
            static_cast<unsigned long long>(loopback_queue_size),
            static_cast<unsigned long long>(loopback_queue_high),
            written_frames, written_fps_milli, duplicate_video_delta,
            static_cast<unsigned long long>(encoder_metrics.input_samples_offered),
            static_cast<unsigned long long>(encoder_metrics.input_samples_accepted),
            static_cast<unsigned long long>(encoder_metrics.input_samples_not_accepted),
            static_cast<unsigned long long>(encoder_metrics.input_samples_deferred_without_event),
            static_cast<unsigned long long>(encoder_metrics.input_errors),
            static_cast<unsigned long long>(encoder_metrics.output_samples_produced),
            static_cast<unsigned long long>(encoder_metrics.output_waits_for_input),
            static_cast<unsigned long long>(encoder_metrics.pending_inputs),
            static_cast<unsigned long long>(encoder_metrics.pending_inputs_high_watermark),
            static_cast<unsigned long long>(encoder_metrics.deferred_inputs_pending),
            static_cast<unsigned long long>(encoder_metrics.deferred_inputs_high_watermark),
            static_cast<unsigned long long>(encoder_metrics.input_samples_retried),
            static_cast<unsigned long long>(encoder_metrics.output_timestamp_errors),
            static_cast<unsigned long long>(encode_timing.samples),
            static_cast<unsigned long long>(encode_timing.samples ? encode_timing.total_us / encode_timing.samples : 0),
            static_cast<unsigned long long>(encode_timing.max_us),
            static_cast<unsigned long long>(encode_timing.p50_upper_us),
            static_cast<unsigned long long>(encode_timing.p95_upper_us),
            static_cast<unsigned long long>(encode_timing.p99_upper_us),
            static_cast<unsigned long long>(video_mux_timing.samples),
            static_cast<unsigned long long>(video_mux_timing.samples ? video_mux_timing.total_us / video_mux_timing.samples : 0),
            static_cast<unsigned long long>(video_mux_timing.max_us),
            static_cast<unsigned long long>(video_mux_timing.p50_upper_us),
            static_cast<unsigned long long>(video_mux_timing.p95_upper_us),
            static_cast<unsigned long long>(video_mux_timing.p99_upper_us),
            static_cast<unsigned long long>(audio_mux_timing.samples),
            static_cast<unsigned long long>(audio_mux_timing.samples ? audio_mux_timing.total_us / audio_mux_timing.samples : 0),
            static_cast<unsigned long long>(audio_mux_timing.max_us),
            static_cast<unsigned long long>(audio_mux_timing.p50_upper_us),
            static_cast<unsigned long long>(audio_mux_timing.p95_upper_us),
            static_cast<unsigned long long>(audio_mux_timing.p99_upper_us),
            static_cast<unsigned long long>(persistent_log_dropped_count()));
        diagnostics_.write_event(resource_event);
        if (muxer_) {
            const auto& writer = writer_live;
            const uint64_t active_write_age_ms =
                writer.active_write_started_ms != 0 && now_ms >= writer.active_write_started_ms
                    ? now_ms - writer.active_write_started_ms : 0;
            diagnostics_.write_event(
                L"event=writer_sample elapsed_s=" +
                std::to_wstring((now_ms - diagnostics_start_ms) / 1000) +
                L" queue_packets=" + std::to_wstring(writer.queue.packet_count) +
                L" queue_high_packets=" + std::to_wstring(writer.queue.packet_high_watermark) +
                L" queue_bytes=" + std::to_wstring(writer.queue.queued_bytes) +
                L" queue_high_bytes=" + std::to_wstring(writer.queue.queued_bytes_high_watermark) +
                L" video_span_100ns=" + std::to_wstring(writer.queue.video_span_100ns) +
                L" audio_span_100ns=" + std::to_wstring(writer.queue.audio_span_100ns) +
                L" accepted=" + std::to_wstring(writer.samples_enqueued) +
                L" written=" + std::to_wstring(writer.samples_written) +
                L" video_written=" + std::to_wstring(writer.video_samples_written) +
                L" audio_written=" + std::to_wstring(writer.audio_samples_written) +
                L" rejected=" + std::to_wstring(writer.queue.rejected_packets) +
                L" discarded=" + std::to_wstring(writer.queue.discarded_packets) +
                L" failures=" + std::to_wstring(writer.write_failures) +
                L" active_write_age_ms=" + std::to_wstring(active_write_age_ms) +
                L" active_sample_bytes=" + std::to_wstring(writer.active_sample_bytes) +
                L" write_avg_us=" + std::to_wstring(writer.samples_written
                    ? writer.write_total_us / writer.samples_written : 0) +
                L" write_max_us=" + std::to_wstring(writer.write_max_us) +
                L" write_p50_us=" + std::to_wstring(writer.write_latency.p50_upper_us) +
                L" write_p95_us=" + std::to_wstring(writer.write_latency.p95_upper_us) +
                L" write_p99_us=" + std::to_wstring(writer.write_latency.p99_upper_us) +
                L" healthy=" + std::to_wstring(writer.failed ? 0 : 1));
        }
        const auto async_wait = encoder_metrics.async_event_wait_us;
        const auto async_queue_wait = encoder_metrics.async_input_queue_residence_us;
        const auto async_input_time = encoder_metrics.async_process_input_us;
        const auto async_output_time = encoder_metrics.async_process_output_us;
        const auto async_output_latency = encoder_metrics.async_accept_to_output_us;
        diagnostics_.write_event(
            L"event=encoder_async_sample elapsed_s=" + std::to_wstring((now_ms - diagnostics_start_ms) / 1000) +
            L" need_input_events=" + std::to_wstring(encoder_metrics.async_need_input_events) +
            L" have_output_events=" + std::to_wstring(encoder_metrics.async_have_output_events) +
            L" worker_wakeups=" + std::to_wstring(encoder_metrics.async_worker_wakeups) +
            L" input_queue_depth=" + std::to_wstring(encoder_metrics.deferred_inputs_pending) +
            L" input_queue_high_watermark=" + std::to_wstring(encoder_metrics.deferred_inputs_high_watermark) +
            L" output_queue_high_watermark=" + std::to_wstring(encoder_metrics.async_output_queue_high_watermark) +
            L" output_buffer_capacity_high_watermark=" +
            std::to_wstring(encoder_metrics.async_output_buffer_capacity_high_watermark) +
            L" input_credit_high_watermark=" + std::to_wstring(encoder_metrics.async_input_credit_high_watermark) +
            L" event_wait_p95_us=" + std::to_wstring(async_wait.p95_upper_us) +
            L" input_queue_residence_p95_us=" + std::to_wstring(async_queue_wait.p95_upper_us) +
            L" process_input_p95_us=" + std::to_wstring(async_input_time.p95_upper_us) +
            L" process_output_p95_us=" + std::to_wstring(async_output_time.p95_upper_us) +
            L" accepted_to_output_p95_us=" + std::to_wstring(async_output_latency.p95_upper_us));
        last_captured_frames = captured_frames;
        last_written_frames = written_frames;
        last_duplicate_frames = live_telemetry.dup_frames;
        last_dropped_frames = dropped_frames;
        last_capture_queue_drops = capture_queue_drops;

        const uint32_t encoded_now = frames_encoded_.load(std::memory_order_relaxed);
        const bool recording = machine_.is_recording();
        if (!recording || encoded_now != last_encoded) {
            last_encoded = encoded_now;
            last_video_progress_ms = now_ms;
        }
        const bool write_blocked = write_started != 0 && now_ms >= write_started &&
                                   now_ms - write_started >= 10'000;
        const bool video_stalled = now_ms >= last_video_progress_ms &&
                                   now_ms - last_video_progress_ms >= 20'000 &&
                                   (audio_written_.load(std::memory_order_relaxed) > 0 ||
                                    (capture_ && capture_->frames_captured() > 0));
        const auto callback_stage = capture_ ? capture_->frame_callback_stage()
                                             : CaptureEngine::FrameCallbackStage::Idle;
        const uint64_t callback_age = capture_ ? capture_->frame_callback_age_ms() : 0;
        const bool callback_stalled = callback_stage != CaptureEngine::FrameCallbackStage::Idle &&
                                      callback_age >= 20'000;
        const uint32_t current_audio_drops = (audio_ ? audio_->queue_drops() : 0) +
                                             (loopback_audio_ ? loopback_audio_->queue_drops() : 0);
        const bool audio_lost = recording && current_audio_drops > last_audio_drops;
        last_audio_drops = current_audio_drops;
        if (!write_stall_reported && recording && (write_blocked || video_stalled || callback_stalled || audio_lost)) {
            write_stall_reported = true;
            pipeline_stall_events_.fetch_add(1, std::memory_order_relaxed);
            if (audio_lost) {
                diagnostics_.write_event(L"event=audio_queue_loss_stop audio_drops=" +
                                         std::to_wstring(current_audio_drops));
            } else if (write_blocked) {
                diagnostics_.write_event(L"event=mux_write_stalled elapsed_ms=" +
                                         std::to_wstring(now_ms - write_started));
            } else if (callback_stalled) {
                diagnostics_.write_event(L"event=capture_callback_stalled stage=" +
                    std::wstring(capture_stage_label(callback_stage)) + L" elapsed_ms=" +
                    std::to_wstring(callback_age));
            } else {
                diagnostics_.write_event(L"event=video_output_stalled elapsed_ms=" +
                                         std::to_wstring(now_ms - last_video_progress_ms));
            }
            notify_error(audio_lost
                ? L"Audio was lost because the recorder could not keep up. Stopping to save the remaining recording."
                : L"Recording output stopped progressing. Stopping to protect the recording; saving may take time.");
            if (on_stop_requested_) on_stop_requested_();
        }
        const auto monitor_now = std::chrono::steady_clock::now();
        if (monitor_now - last_heartbeat < kHeartbeatInterval) {
            wait_lock.lock();
            continue;
        }
        last_heartbeat = monitor_now;
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            monitor_now - diagnostics_started_).count();
        const auto stage = diagnostics_stage_.load(std::memory_order_relaxed);
        const uint32_t captured = capture_ ? capture_->frames_captured() : 0;
        const uint32_t encoded = frames_encoded_.load(std::memory_order_relaxed);
        const uint32_t dropped = capture_ ? capture_->frames_dropped() : 0;
        const uint32_t queue_drops = capture_ ? capture_->queue_drops() : 0;
        const size_t video_queue = frame_queue_ ? frame_queue_->size() : 0;
        const size_t mic_queue = audio_queue_ ? audio_queue_->size() : 0;
        const size_t loopback_queue = loopback_queue_ ? loopback_queue_->size() : 0;
        const uint32_t audio_drops = (audio_ ? audio_->queue_drops() : 0) +
                                     (loopback_audio_ ? loopback_audio_->queue_drops() : 0);
        const uint32_t audio_packets = audio_written_.load(std::memory_order_relaxed);
        const auto capture_stage = capture_
            ? capture_->frame_callback_stage()
            : CaptureEngine::FrameCallbackStage::Idle;
        const uint64_t capture_stage_age_ms = capture_ ? capture_->frame_callback_age_ms() : 0;
        uint64_t partial_bytes = 0;
        WIN32_FILE_ATTRIBUTE_DATA file_data{};
        if (GetFileAttributesExW(current_partial_path_.c_str(), GetFileExInfoStandard, &file_data)) {
            ULARGE_INTEGER size{};
            size.HighPart = file_data.nFileSizeHigh;
            size.LowPart = file_data.nFileSizeLow;
            partial_bytes = size.QuadPart;
        }

        wchar_t event[1024]{};
        _snwprintf_s(event, _countof(event), _TRUNCATE,
                     L"event=heartbeat elapsed_s=%lld stage=%s "
                     L"captured=%u encoded=%u dropped=%u queue_drops=%u "
                     L"video_queue=%llu mic_queue=%llu loopback_queue=%llu "
                     L"audio_drops=%u audio_packets=%u partial_bytes=%llu "
                     L"video_pts_100ns=%lld audio_pts_100ns=%lld "
                     L"capture_callback=%s capture_callback_age_ms=%llu",
                     static_cast<long long>(elapsed), diagnostics_stage_label(stage),
                     captured, encoded, dropped, queue_drops,
                     static_cast<unsigned long long>(video_queue),
                     static_cast<unsigned long long>(mic_queue),
                     static_cast<unsigned long long>(loopback_queue),
                     audio_drops, audio_packets,
                     static_cast<unsigned long long>(partial_bytes),
                     static_cast<long long>(muxer_ ? muxer_->last_video_pts() : -1),
                     static_cast<long long>(muxer_ ? muxer_->last_audio_pts() : -1),
                     capture_stage_label(capture_stage),
                     static_cast<unsigned long long>(capture_stage_age_ms));
        diagnostics_.write_event(event);
        wait_lock.lock();
    }
}

bool SessionController::pause() {
    if (!machine_.transition(SessionEvent::Pause)) return false;
    sync_.pause();
    pacer_reset_requested_.store(true, std::memory_order_release);
    notify_status(L"Paused");
    return true;
}

bool SessionController::resume() {
    if (!machine_.transition(SessionEvent::Resume)) return false;
    sync_.resume();
    pacer_reset_requested_.store(true, std::memory_order_release);
    // Force an IDR keyframe on the next encoded frame so the resumed segment
    // is independently decodable and seeks work correctly after pause gaps.
    encoder_->request_keyframe();
    notify_status(L"Recording...");
    return true;
}

void SessionController::set_muted(bool muted) {
    audio_->set_muted(muted);
}

bool SessionController::is_muted() const {
    return audio_->is_muted();
}

uint32_t SessionController::frames_captured()       const { return capture_->frames_captured(); }
uint32_t SessionController::frames_dropped()        const { return capture_->frames_dropped(); }
uint32_t SessionController::frames_encoded()        const { return frames_encoded_.load(); }
uint32_t SessionController::audio_packets_written() const { return audio_written_.load(); }
uint32_t SessionController::active_fps() const {
    return capture_ ? capture_->target_fps()
                    : (encoder_ ? encoder_->output_fps() : 0);
}

EncoderProfile SessionController::active_encoder_profile() const {
    EncoderProfile profile{};
    if (encoder_) {
        profile.width = encoder_->output_width();
        profile.height = encoder_->output_height();
        profile.bitrate_bps = encoder_->output_bitrate();
        profile.fps = active_fps();
    }
    return profile;
}

// T037: Build a full telemetry snapshot for the debug overlay
TelemetrySnapshot SessionController::telemetry_snapshot() const {
    uint32_t enc_mode = 0;
    if (encoder_) {
        switch (encoder_->mode()) {
            case EncoderMode::HardwareMFT:   enc_mode = 0; break;
            case EncoderMode::SoftwareMFT:   enc_mode = 1; break;
            case EncoderMode::SoftwareMFT720p: enc_mode = 2; break;
        }
    }
    // Update backlog counter from the live queue depth
    uint32_t backlog = frame_queue_ ? static_cast<uint32_t>(frame_queue_->size()) : 0;
    const_cast<TelemetryStore&>(telemetry_).set_backlog(backlog);
    auto snapshot = telemetry_.snapshot(enc_mode, last_power_ac_.load(std::memory_order_acquire));
    if (capture_) {
        snapshot.frames_captured = capture_->frames_captured();
        snapshot.frames_dropped += capture_->frames_dropped();
        snapshot.queue_drops = capture_->queue_drops();
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// Encode loop — runs on encode_thread_
// Drains both frame_queue_ and audio_queue_, feeds encoder + muxer
// T038: FramePacer normalises jittery WGC timestamps
// ---------------------------------------------------------------------------
void SessionController::encode_loop() {
    // Register this thread with MMCSS for higher priority
    // (same priority class as capture)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // T038: keep a copy of the last encoded frame's texture for duplicate insertion.
    // RenderFrame is move-only; store the texture ComPtr separately (AddRef on copy).
    ComPtr<ID3D11Texture2D> last_texture;
    std::shared_ptr<TextureSurfaceLease> last_surface_lease;
    bool        have_last_frame  = false;
    int64_t     last_paced_pts   = 0;
    ULONGLONG   last_power_check_ms = 0;
    constexpr ULONGLONG kPowerCheckIntervalMs = 10'000;
    constexpr uint32_t kAudioOutputRate = 48'000;

    AudioTimelineMixer audio_mixer;

    auto encode_frame_timed = [this](ID3D11Texture2D* texture, int64_t pts,
                                     std::shared_ptr<TextureSurfaceLease> surface_lease,
                                     ComPtr<IMFSample>& output, bool synthetic) {
        const auto before = encoder_->diagnostics_snapshot();
        const auto started = std::chrono::steady_clock::now();
        const bool produced = encoder_->encode_frame(
            texture, pts, output, std::move(surface_lease), synthetic);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        video_encode_timing_.record(static_cast<uint64_t>((std::max)(0LL, elapsed_us)));
        const auto after = encoder_->diagnostics_snapshot();
        const uint64_t abandoned_delta = after.pending_inputs_abandoned - before.pending_inputs_abandoned;
        const uint64_t preparation_delta = after.input_preparation_errors - before.input_preparation_errors;
        const uint64_t input_error_delta = after.input_errors - before.input_errors;
        const uint64_t output_error_delta = after.output_errors - before.output_errors;
        const uint64_t scheduler_error_delta = after.scheduler_errors - before.scheduler_errors;
        const uint64_t accounting_error_delta = after.input_accounting_errors - before.input_accounting_errors;
        const uint64_t timestamp_error_delta = after.output_timestamp_errors - before.output_timestamp_errors;
        if (abandoned_delta != 0 || preparation_delta != 0 || input_error_delta != 0 ||
            output_error_delta != 0 || scheduler_error_delta != 0 ||
            accounting_error_delta != 0 || timestamp_error_delta != 0) {
            diagnostics_.write_event(L"event=encoder_frame_failure abandoned=" +
                std::to_wstring(abandoned_delta) + L" preparation=" +
                std::to_wstring(preparation_delta) + L" input=" +
                std::to_wstring(input_error_delta) + L" output=" +
                std::to_wstring(output_error_delta) + L" scheduler=" +
                std::to_wstring(scheduler_error_delta) + L" accounting=" +
                std::to_wstring(accounting_error_delta) + L" timestamp=" +
                std::to_wstring(timestamp_error_delta));
            report_recording_loss(L"encoder_input");
        }
        return produced;
    };
    auto write_video_timed = [this](IMFSample* sample) {
        const auto started = std::chrono::steady_clock::now();
        const bool written = muxer_->write_video(sample);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        video_mux_timing_.record(static_cast<uint64_t>((std::max)(0LL, elapsed_us)));
        return written;
    };
    auto account_video_write = [this, &write_video_timed](IMFSample* sample, bool synthetic,
                                                           uint64_t frame_id = 0) {
        if (!sample) return false;
        if (write_video_timed(sample)) {
            frames_encoded_.fetch_add(1, std::memory_order_relaxed);
            telemetry_.on_frame_encoded();
            if (synthetic) telemetry_.on_duplicate_inserted();
            return true;
        }
        const uint32_t failures = video_write_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures == 1 || (failures % 120) == 0) {
            SR_LOG_ERROR(L"Muxer rejected encoded video sample (frame_id=%llu total failures=%u)",
                         static_cast<unsigned long long>(frame_id), failures);
        }
        diagnostics_.write_event(L"event=encoded_video_write_failed frame_id=" +
                                 std::to_wstring(frame_id) +
                                 L" synthetic=" + std::to_wstring(synthetic ? 1 : 0));
        report_recording_loss(L"video_mux");
        return false;
    };
    bool async_encoder_failure_reported = false;
    bool async_writer_failure_reported = false;
    auto drain_async_outputs = [this, &account_video_write, &async_encoder_failure_reported,
                               &async_writer_failure_reported]() {
        VideoEncoder::AsyncEncodedOutput output;
        while (encoder_->try_pop_async_output(output)) {
            account_video_write(output.sample.Get(), output.synthetic, output.frame_id);
        }
        if (encoder_->async_worker_failed() && !async_encoder_failure_reported) {
            async_encoder_failure_reported = true;
            diagnostics_.write_event(L"event=async_encoder_worker_failed");
            report_recording_loss(L"encoder_async_worker");
        }
        if (muxer_ && !muxer_->healthy() && !async_writer_failure_reported) {
            async_writer_failure_reported = true;
            diagnostics_.write_event(L"event=media_writer_worker_failed");
            report_recording_loss(L"media_writer_worker");
        }
    };
    auto write_audio_timed = [this](IMFSample* sample) {
        const auto started = std::chrono::steady_clock::now();
        const bool written = muxer_->write_audio(sample);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        audio_mux_timing_.record(static_cast<uint64_t>((std::max)(0LL, elapsed_us)));
        return written;
    };

    auto acquire_audio_sample = [](DWORD required_bytes,
                                   ComPtr<IMFSample>& out_sample,
                                   ComPtr<IMFMediaBuffer>& out_buffer) {
        const HRESULT hr = create_audio_sample(required_bytes, &out_sample, &out_buffer);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Creating independent audio sample (%u bytes) failed: 0x%08X",
                         required_bytes, hr);
            return false;
        }
        return true;
    };

    auto write_mixed_audio = [this, &acquire_audio_sample, &write_audio_timed](
                                 const MixedAudioBlock& mixed) {
        if (mixed.stereo.size() > MAXDWORD / sizeof(float)) {
            SR_LOG_ERROR(L"Mixed audio block is too large: %zu samples", mixed.stereo.size());
            return false;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD bytes = static_cast<DWORD>(mixed.stereo.size() * sizeof(float));
        if (!acquire_audio_sample(bytes, sample, buffer)) return false;

        BYTE* data = nullptr;
        HRESULT hr = buffer->Lock(&data, nullptr, nullptr);
        if (FAILED(hr) || !data) {
            SR_LOG_ERROR(L"Mixed audio buffer lock failed: 0x%08X", hr);
            return false;
        }
        std::memcpy(data, mixed.stereo.data(), bytes);
        hr = buffer->Unlock();
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Mixed audio buffer unlock failed: 0x%08X", hr);
            return false;
        }
        hr = buffer->SetCurrentLength(bytes);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Mixed audio SetCurrentLength failed: 0x%08X", hr);
            return false;
        }
        hr = sample->SetSampleTime(mixed.pts);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Mixed audio SetSampleTime failed: 0x%08X", hr);
            return false;
        }
        hr = sample->SetSampleDuration(
            static_cast<int64_t>(mixed.frames) * 10'000'000LL /
            static_cast<int64_t>(kAudioOutputRate));
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Mixed audio SetSampleDuration failed: 0x%08X", hr);
            return false;
        }

        diagnostics_stage_.store(DiagnosticsStage::MuxingAudio, std::memory_order_relaxed);
        if (write_audio_timed(sample.Get())) {
            audio_written_.fetch_add(1, std::memory_order_relaxed);
            telemetry_.on_audio_written();
            return true;
        }
        audio_write_failures_.fetch_add(1, std::memory_order_relaxed);
        report_recording_loss(L"audio_mux");
        return false;
    };

    // Interleave video and audio: write video first, then any pending audio
    while (encode_running_.load(std::memory_order_acquire) ||
           !frame_queue_->empty() || !audio_queue_->empty() || !loopback_queue_->empty())
    {
        drain_async_outputs();
        if (pacer_reset_requested_.exchange(false, std::memory_order_acq_rel)) {
            pacer_.reset();
        }
        diagnostics_stage_.store(DiagnosticsStage::WaitingForFrame, std::memory_order_relaxed);
        uint32_t target_fps = encoder_ ? encoder_->output_fps() : 24;
        auto wait_interval = std::chrono::milliseconds(
            500 / (std::max)(1u, target_fps));

        // --- Video ---
        if (auto opt_frame = frame_queue_->wait_pop(wait_interval)) {
            auto& frame = *opt_frame;

            // Skip frames while paused
            if (machine_.is_paused() ||
                stopping_from_pause_.load(std::memory_order_acquire)) {
                continue;
            }

            // T038: pace the incoming PTS and decide what to do
            // We already popped one frame, so this queue cannot be full here.
            bool   queue_full = false;
            int64_t paced_pts = frame.pts;
            PaceAction action = pacer_.pace_frame(frame.pts, queue_full, &paced_pts);

            if (action == PaceAction::Drop) {
                // Backpressure drop — discard this frame
                telemetry_.on_frame_dropped();
                continue;
            }

            // T038: duplicate — encode the last frame again with a synthetic PTS
            if (action == PaceAction::Duplicate && have_last_frame && last_texture) {
                // Duplicate PTS = midpoint between last and current frame.
                int64_t dup_pts = last_paced_pts + (paced_pts - last_paced_pts) / 2;
                ComPtr<IMFSample> dup_sample;
                diagnostics_stage_.store(DiagnosticsStage::EncodingVideo, std::memory_order_relaxed);
                if (encode_frame_timed(last_texture.Get(), dup_pts, last_surface_lease,
                                       dup_sample, true) && dup_sample) {
                    diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                    account_video_write(dup_sample.Get(), true, 0);
                }
                drain_async_outputs();
            }

            // Cache this texture (ComPtr copy AddRefs it) before encoding
            last_texture = frame.texture;
            last_surface_lease = frame.surface_lease;

            // Encode current frame
            ComPtr<IMFSample> encoded;
            diagnostics_stage_.store(DiagnosticsStage::EncodingVideo, std::memory_order_relaxed);
            if (encode_frame_timed(frame.texture.Get(), paced_pts, frame.surface_lease, encoded, false) && encoded) {
                diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                account_video_write(encoded.Get(), false, 0);
            }
            drain_async_outputs();

            have_last_frame = true;
            last_paced_pts  = paced_pts;
        } else if (machine_.is_recording() && have_last_frame && last_texture &&
                   encode_running_.load(std::memory_order_acquire)) {
            // WGC can stop delivering frames when the desktop is unchanged.
            // Keep the video timeline advancing so the sink sees both streams
            // progress and the last desktop image remains visible.
            const int64_t interval = 10'000'000LL / (std::max)(1u, target_fps);
            const int64_t now_pts = sync_.now_pts();
            const int64_t next_pts = last_paced_pts + interval;
            if (now_pts >= next_pts + interval / 2) {
                const int64_t repeat_pts = idle_repeat_pts(now_pts, last_paced_pts, interval);
                int64_t paced_pts = repeat_pts;
                pacer_.pace_frame(repeat_pts, false, &paced_pts);
                ComPtr<IMFSample> encoded;
                diagnostics_stage_.store(DiagnosticsStage::EncodingVideo, std::memory_order_relaxed);
                if (encode_frame_timed(last_texture.Get(), paced_pts, last_surface_lease,
                                       encoded, true) && encoded) {
                    diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                    account_video_write(encoded.Get(), true, 0);
                }
                drain_async_outputs();
                last_paced_pts = paced_pts;
            }
        }

        // --- Audio: timestamp-align mic and loopback, mix, and write one stream ---
        {
            diagnostics_stage_.store(DiagnosticsStage::MixingAudio, std::memory_order_relaxed);
            while (auto packet = audio_queue_->try_pop()) {
                if (!machine_.is_paused() &&
                    !stopping_from_pause_.load(std::memory_order_acquire)) {
                    const auto result = audio_mixer.push(std::move(*packet), true);
                    if (result.dropped_packets != 0 || result.rejected) {
                        diagnostics_.write_event(L"event=audio_mixer_overflow source=microphone dropped=" +
                            std::to_wstring(result.dropped_packets) + L" rejected=" +
                            std::to_wstring(result.rejected ? 1 : 0));
                        report_recording_loss(L"audio_mixer_queue");
                    }
                }
            }
            while (auto packet = loopback_queue_->try_pop()) {
                if (!machine_.is_paused() &&
                    !stopping_from_pause_.load(std::memory_order_acquire)) {
                    const auto result = audio_mixer.push(std::move(*packet), false);
                    if (result.dropped_packets != 0 || result.rejected) {
                        diagnostics_.write_event(L"event=audio_mixer_overflow source=loopback dropped=" +
                            std::to_wstring(result.dropped_packets) + L" rejected=" +
                            std::to_wstring(result.rejected ? 1 : 0));
                        report_recording_loss(L"audio_mixer_queue");
                    }
                }
            }

            // A fixed four blocks per video write cannot catch up after a slow
            // write. Advance audio towards the last submitted video timestamp,
            // with a hard bound to avoid starving video on large gaps.
            for (unsigned blocks = 0; blocks < 200; ++blocks) {
                auto mixed = audio_mixer.take();
                if (!mixed) break;
                if (!write_mixed_audio(*mixed)) {
                    audio_write_failures_.fetch_add(1, std::memory_order_relaxed);
                }
                if (!continue_audio_pass(blocks + 1, mixed->pts,
                                         last_paced_pts, have_last_frame)) break;
            }
            diagnostics_stage_.store(DiagnosticsStage::WaitingForFrame, std::memory_order_relaxed);
        }

        const ULONGLONG now_ms = GetTickCount64();

        // Dynamic power monitoring — check every 10 seconds.
        // Keep this timestamp session-local so each recording starts fresh.
        if (now_ms - last_power_check_ms >= kPowerCheckIntervalMs) {
            bool on_ac = PowerModeDetector::is_on_ac_power();
            if (on_ac != last_power_ac_.load(std::memory_order_acquire)) {
                last_power_ac_.store(on_ac, std::memory_order_release);
                SR_LOG_INFO(L"[Power] Switched to %s power", on_ac ? L"AC" : L"Battery");
                // Keep capture work within the active power budget. The
                // encoder's sample timestamps remain on the source clock.
                const uint32_t new_fps = on_ac
                    ? encoder_->output_fps()
                    : (active_high_quality_
                        ? encoder_->output_fps()
                        : (std::min)(encoder_->output_fps(),
                                     PowerModeDetector::kBatteryMaxFps));
                capture_->set_target_fps(new_fps);
                SR_LOG_INFO(L"[Power] Capture cadence changed to %u fps", new_fps);
            }
            last_power_check_ms = now_ms;
        }

    }

    // Capture is stopped before this thread is joined; flush the mixer reorder
    // window so the final microphone/system audio samples reach the MP4.
    while (auto mixed = audio_mixer.take(true)) {
        if (!write_mixed_audio(*mixed)) {
            audio_write_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    audio_mixer_stats_ = audio_mixer.stats();
}

// ---------------------------------------------------------------------------
void SessionController::notify_status(const std::wstring& msg) {
    if (on_status_) on_status_(msg);
}
void SessionController::notify_error(const std::wstring& msg) {
    SR_LOG_ERROR(L"%s", msg.c_str());
    if (on_error_) on_error_(msg);
}

void SessionController::report_recording_loss(const wchar_t* stage) {
    if (output_failure_reported_.exchange(true, std::memory_order_acq_rel)) return;
    diagnostics_.write_event(L"event=recording_loss stage=" + std::wstring(stage));
    notify_error(L"Recording detected unrecoverable media loss. Stopping; review the saved file and diagnostics.");
    if ((machine_.is_recording() || machine_.is_paused()) && on_stop_requested_) {
        on_stop_requested_();
    }
}

} // namespace sr
