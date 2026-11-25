// session_controller.cpp — Wires all engines together into the recording pipeline
// T016: Start -> init engines -> run capture->encode->mux loop -> Stop -> finalize

#include "controller/session_controller.h"
#include "controller/audio_write_schedule.h"
#include "capture/capture_engine.h"
#include "audio/audio_engine.h"
#include "audio/audio_mixer.h"
#include "encoder/video_encoder.h"
#include "storage/mux_writer.h"
#include "storage/storage_manager.h"
#include "utils/logging.h"
#include "utils/qpc_clock.h"

#include <mfapi.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
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
        audio_->stop();
        loopback_audio_->stop();
        if (encode_thread_.joinable()) encode_thread_.join();
        muxer_->finalize();
    }
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

    // Initialize Media Foundation
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"MFStartup failed: 0x%08X", hr);
        return false;
    }

    // Probe D3D11 + HW encoder
    if (!EncoderProbe::run(probe_)) {
        notify_error(L"D3D11 initialization failed");
        return false;
    }

    SR_LOG_INFO(L"SessionController initialized. Adapter: %s, HW encoder: %s",
        probe_.adapter_name.c_str(),
        probe_.hw_encoder_available ? probe_.encoder_name.c_str() : L"not available"
    );
    return true;
}

bool SessionController::start() {
    if (!machine_.transition(SessionEvent::Start)) return false;

    diagnostics_stage_.store(DiagnosticsStage::Starting, std::memory_order_relaxed);

    // Elevate process priority during recording for smooth capture
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    notify_status(L"Starting...");

    // ---------------------------------------------------------------
    // Determine output paths
    // generateFilename() returns the .partial.mp4 path
    // ---------------------------------------------------------------
    current_partial_path_ = storage_ ? storage_->generateFilename() : L"ScreenRec.partial.mp4";
    current_output_path_  = StorageManager::partialToFinal(current_partial_path_);
    if (diagnostics_.open_for_output(current_output_path_)) {
        SR_LOG_INFO(L"Diagnostics log -> %s", diagnostics_.path().c_str());
    } else {
        SR_LOG_WARN(L"Diagnostics log could not be opened for: %s", current_output_path_.c_str());
    }

    // ---------------------------------------------------------------
    // Anchor sync clock
    // ---------------------------------------------------------------
    sync_.start();

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
    last_power_ac_ = PowerModeDetector::is_on_ac_power();
    enc_prof = PowerModeDetector::clamp_for_quality_and_power_state(
        enc_prof, last_power_ac_, high_quality_profile);
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
        diagnostics_.write_failure(L"capture_engine_initialization_failed");
        clear_persistent_log_path();
        notify_error(L"Capture engine initialization failed");
        machine_.transition(SessionEvent::Stop);
        machine_.transition(SessionEvent::Finalized);
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
        diagnostics_.write_failure(L"video_encoder_initialization_failed");
        clear_persistent_log_path();
        notify_error(L"Video encoder initialization failed");
        machine_.transition(SessionEvent::Stop);
        machine_.transition(SessionEvent::Finalized);
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
        diagnostics_.write_failure(L"mux_writer_initialization_failed");
        clear_persistent_log_path();
        notify_error(L"Mux writer initialization failed");
        machine_.transition(SessionEvent::Stop);
        machine_.transition(SessionEvent::Finalized);
        return false;
    }

    SessionDiagnostics::StartInfo diagnostics_start;
    diagnostics_start.output_path = current_output_path_;
    diagnostics_start.adapter_name = probe_.adapter_name;
    diagnostics_start.probed_encoder_name =
        probe_.hw_encoder_available ? probe_.encoder_name : L"not available";
    diagnostics_start.encoder_mode = encoder_mode_label(encoder_->mode());
    diagnostics_start.power_state = last_power_ac_ ? L"AC" : L"Battery";
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
    output_failure_reported_.store(false, std::memory_order_relaxed);
    audio_written_.store(0,  std::memory_order_relaxed);
    capture_->reset_counters();
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
    encode_thread_ = std::thread(&SessionController::encode_loop, this);

    if (!capture_->start()) {
        diagnostics_.write_event(L"event=start_stage stage=capture state=failed");
        notify_error(L"Capture start failed");
        stop();
        return false;
    }
    diagnostics_.write_event(L"event=start_stage stage=capture state=complete");
    diagnostics_.write_event(audio_->start()
        ? L"event=start_stage stage=microphone state=complete"
        : L"event=start_stage stage=microphone state=unavailable");
    diagnostics_.write_event(loopback_audio_->start()
        ? L"event=start_stage stage=loopback state=complete"
        : L"event=start_stage stage=loopback state=unavailable");

    // T031: Start async disk space polling — auto-stop on < 500 MB free
    if (storage_) {
        storage_->startDiskSpacePolling([this]() {
            // Runs on poll thread — use atomic transition to safely trigger stop
            if (!machine_.is_idle()) {
                SR_LOG_WARN(L"Auto-stopping: disk space critically low");
                notify_error(L"\u26A0 Disk space critically low! Recording auto-stopped.");
                stop(); // safe: atomic state transitions; stopDiskSpacePolling detaches
            }
        });
    }

    start_diagnostics_monitor();
    diagnostics_.write_event(L"event=recording_started");
    notify_status(L"Recording...");
    SR_LOG_INFO(L"Recording started -> %s", current_output_path_.c_str());
    return true;
}

bool SessionController::stop() {
    bool was_recording = machine_.is_recording() || machine_.is_paused();

    if (!machine_.transition(SessionEvent::Stop)) return false;
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
    if (was_recording) {
        diagnostics_stage_.store(DiagnosticsStage::FlushingEncoder, std::memory_order_relaxed);
        diagnostics_.write_event(L"event=stop_stage stage=encoder_flush state=begin");
        std::vector<ComPtr<IMFSample>> leftover;
        encoder_->flush(leftover);
        diagnostics_.write_event(L"event=stop_stage stage=encoder_flush state=complete samples=" +
                                 std::to_wstring(leftover.size()));
        SR_LOG_INFO(L"Encoder flush produced %zu samples", leftover.size());
        for (auto& s : leftover) {
            if (muxer_->write_video(s.Get())) {
                frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                telemetry_.on_frame_encoded();
            } else {
                const uint32_t failures = video_write_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failures == 1 || (failures % 120) == 0) {
                    SR_LOG_ERROR(L"Muxer rejected encoded video sample (total failures=%u)", failures);
                }
                report_output_failure(L"video");
            }
        }
    }

    // Finalize mux (rename .partial.mp4 -> .mp4)
    diagnostics_stage_.store(DiagnosticsStage::FinalizingMp4, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_stage stage=finalize_mp4 state=begin");
    const bool finalized = muxer_->finalize();
    diagnostics_.write_event(finalized
        ? L"event=stop_stage stage=finalize_mp4 state=complete"
        : L"event=stop_stage stage=finalize_mp4 state=failed");
    if (!finalized) {
        notify_error(L"Failed to finalize recording file. Partial file kept.");
    }

    machine_.transition(SessionEvent::Finalized);
    notify_status(L"Idle");

    // Revert process priority to normal when not recording
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    SessionDiagnostics::StopInfo diagnostics_stop;
    diagnostics_stop.status = finalized ? L"complete" : L"finalize_failed_partial_kept";
    diagnostics_stop.encoder_mode = encoder_mode_label(encoder_->mode());
    diagnostics_stop.frames_captured = capture_->frames_captured();
    diagnostics_stop.frames_encoded = frames_encoded_.load();
    diagnostics_stop.frames_dropped = capture_->frames_dropped();
    diagnostics_stop.queue_drops = capture_->queue_drops();
    diagnostics_stop.video_write_failures = video_write_failures_.load();
    diagnostics_stop.audio_queue_drops = audio_->queue_drops() + loopback_audio_->queue_drops();
    diagnostics_stop.audio_packets = audio_written_.load();
    diagnostics_.write_stop(diagnostics_stop);

    SR_LOG_INFO(L"Recording stopped. Encoded: %u frames, audio pkts: %u",
                frames_encoded_.load(), audio_written_.load());
    diagnostics_stage_.store(DiagnosticsStage::Complete, std::memory_order_relaxed);
    diagnostics_.write_event(L"event=stop_complete");
    stop_diagnostics_monitor();
    clear_persistent_log_path();
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

    while (diagnostics_monitor_running_.load(std::memory_order_acquire)) {
        if (diagnostics_wait_cv_.wait_for(wait_lock, kMonitorInterval, [this]() {
                return !diagnostics_monitor_running_.load(std::memory_order_acquire);
            })) {
            break;
        }

        wait_lock.unlock();
        const uint64_t write_started = muxer_ ? muxer_->active_write_started_ms() : 0;
        const uint64_t now_ms = GetTickCount64();
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
    pacer_.reset(); // T038: avoid treating the pause gap as a frame skip on resume
    notify_status(L"Paused");
    return true;
}

bool SessionController::resume() {
    if (!machine_.transition(SessionEvent::Resume)) return false;
    sync_.resume();
    pacer_.reset(); // T038: fresh pacing baseline after resume
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
    auto snapshot = telemetry_.snapshot(enc_mode, last_power_ac_);
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
    bool        have_last_frame  = false;
    int64_t     last_paced_pts   = 0;
    ULONGLONG   last_mem_sample_ms = 0;
    ULONGLONG   last_power_check_ms = 0;
    constexpr ULONGLONG kPowerCheckIntervalMs = 10'000;
    constexpr uint32_t kAudioOutputRate = 48'000;

    struct ReusableAudioSample {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        DWORD capacity = 0;
    };
    std::array<ReusableAudioSample, 8> audio_sample_pool{};
    size_t audio_pool_cursor = 0;
    AudioTimelineMixer audio_mixer;

    auto has_external_refs = [](IUnknown* obj) -> bool {
        if (!obj) return false;
        const ULONG refs = obj->AddRef();
        obj->Release();
        // refs includes this temporary AddRef and the pool's ComPtr reference.
        return refs > 2;
    };

    auto acquire_audio_sample = [&](DWORD required_bytes,
                                    ComPtr<IMFSample>& out_sample,
                                    ComPtr<IMFMediaBuffer>& out_buffer) -> bool {
        const size_t pool_size = audio_sample_pool.size();
        for (size_t attempt = 0; attempt < pool_size; ++attempt) {
            const size_t idx = (audio_pool_cursor + attempt) % pool_size;
            auto& slot = audio_sample_pool[idx];
            const bool reusable = !slot.sample ||
                (!has_external_refs(slot.sample.Get()) && !has_external_refs(slot.buffer.Get()));
            if (!reusable) continue;

            HRESULT hr = S_OK;
            if (!slot.sample) {
                hr = MFCreateSample(&slot.sample);
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"MFCreateSample (pooled audio) failed: 0x%08X", hr);
                    return false;
                }
            }
            if (!slot.buffer || slot.capacity < required_bytes) {
                slot.buffer.Reset();
                hr = MFCreateMemoryBuffer(required_bytes, &slot.buffer);
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"MFCreateMemoryBuffer (pooled audio, %u bytes) failed: 0x%08X",
                                 required_bytes, hr);
                    return false;
                }
                slot.capacity = required_bytes;
            }

            slot.sample->RemoveAllBuffers();
            hr = slot.sample->AddBuffer(slot.buffer.Get());
            if (FAILED(hr)) {
                SR_LOG_ERROR(L"Audio sample AddBuffer failed: 0x%08X", hr);
                return false;
            }

            out_sample = slot.sample;
            out_buffer = slot.buffer;
            audio_pool_cursor = (idx + 1) % pool_size;
            return true;
        }

        // Fallback: transient allocation when all pooled samples are in flight.
        HRESULT hr = MFCreateSample(&out_sample);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"MFCreateSample (transient audio) failed: 0x%08X", hr);
            return false;
        }
        hr = MFCreateMemoryBuffer(required_bytes, &out_buffer);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"MFCreateMemoryBuffer (transient audio, %u bytes) failed: 0x%08X",
                         required_bytes, hr);
            return false;
        }
        hr = out_sample->AddBuffer(out_buffer.Get());
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"Audio transient AddBuffer failed: 0x%08X", hr);
            return false;
        }
        return true;
    };

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

    // Interleave video and audio: write video first, then any pending audio
    while (encode_running_.load(std::memory_order_acquire) ||
           !frame_queue_->empty() || !audio_queue_->empty() || !loopback_queue_->empty())
    {
        diagnostics_stage_.store(DiagnosticsStage::WaitingForFrame, std::memory_order_relaxed);
        uint32_t target_fps = encoder_ ? encoder_->output_fps() : 24;
        auto wait_interval = std::chrono::milliseconds(
            500 / (std::max)(1u, target_fps));

        // --- Video ---
        if (auto opt_frame = frame_queue_->wait_pop(wait_interval)) {
            auto& frame = *opt_frame;

            // Skip frames while paused
            if (machine_.is_paused()) {
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
                if (encoder_->encode_frame(last_texture.Get(), dup_pts, dup_sample)) {
                    diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                    if (muxer_->write_video(dup_sample.Get())) {
                        frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                        telemetry_.on_frame_encoded();
                        telemetry_.on_duplicate_inserted();
                    } else {
                        const uint32_t failures = video_write_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (failures == 1 || (failures % 120) == 0) {
                            SR_LOG_ERROR(L"Muxer rejected duplicate video sample (total failures=%u)", failures);
                        }
                        report_output_failure(L"video");
                    }
                }
            }

            // Cache this texture (ComPtr copy AddRefs it) before encoding
            last_texture = frame.texture;

            // Encode current frame
            ComPtr<IMFSample> encoded;
            diagnostics_stage_.store(DiagnosticsStage::EncodingVideo, std::memory_order_relaxed);
            if (encoder_->encode_frame(frame.texture.Get(), paced_pts, encoded)) {
                diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                if (muxer_->write_video(encoded.Get())) {
                    frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                    telemetry_.on_frame_encoded();
                } else {
                    const uint32_t failures = video_write_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (failures == 1 || (failures % 120) == 0) {
                        SR_LOG_ERROR(L"Muxer rejected video sample (total failures=%u)", failures);
                    }
                    report_output_failure(L"video");
                }
            }

            have_last_frame = true;
            last_paced_pts  = paced_pts;
        } else if (machine_.is_recording() && have_last_frame && last_texture &&
                   encode_running_.load(std::memory_order_acquire)) {
            // WGC can stop delivering frames when the desktop is unchanged.
            // Keep the video timeline advancing so the sink sees both streams
            // progress and the last desktop image remains visible.
            const int64_t interval = 10'000'000LL / (std::max)(1u, target_fps);
            const int64_t now_pts = QPCClock::instance().now_hns() - sync_.anchor_100ns();
            const int64_t next_pts = last_paced_pts + interval;
            if (now_pts >= next_pts + interval / 2) {
                const int64_t repeat_pts = idle_repeat_pts(now_pts, last_paced_pts, interval);
                int64_t paced_pts = repeat_pts;
                pacer_.pace_frame(repeat_pts, false, &paced_pts);
                ComPtr<IMFSample> encoded;
                diagnostics_stage_.store(DiagnosticsStage::EncodingVideo, std::memory_order_relaxed);
                if (encoder_->encode_frame(last_texture.Get(), paced_pts, encoded)) {
                    diagnostics_stage_.store(DiagnosticsStage::MuxingVideo, std::memory_order_relaxed);
                    if (muxer_->write_video(encoded.Get())) {
                        frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                        telemetry_.on_frame_encoded();
                        telemetry_.on_duplicate_inserted();
                    } else {
                        video_write_failures_.fetch_add(1, std::memory_order_relaxed);
                        report_output_failure(L"video");
                    }
                }
                last_paced_pts = paced_pts;
            }
        }

        // --- Audio: timestamp-align mic and loopback, mix, and write one stream ---
        {
            diagnostics_stage_.store(DiagnosticsStage::MixingAudio, std::memory_order_relaxed);
            while (auto packet = audio_queue_->try_pop()) {
                if (!machine_.is_paused()) audio_mixer.push(std::move(*packet), true);
            }
            while (auto packet = loopback_queue_->try_pop()) {
                if (!machine_.is_paused()) audio_mixer.push(std::move(*packet), false);
            }

            // A fixed four blocks per video write cannot catch up after a slow
            // write. Advance audio towards the last submitted video timestamp,
            // with a hard bound to avoid starving video on large gaps.
            for (unsigned blocks = 0; blocks < 200; ++blocks) {
                auto mixed = audio_mixer.take();
                if (!mixed) break;
                ComPtr<IMFSample> sample;
                ComPtr<IMFMediaBuffer> buffer;
                const DWORD bytes = static_cast<DWORD>(mixed->stereo.size() * sizeof(float));
                if (acquire_audio_sample(bytes, sample, buffer)) {
                    BYTE* data = nullptr;
                    HRESULT hr = buffer->Lock(&data, nullptr, nullptr);
                    if (SUCCEEDED(hr) && data) {
                        std::memcpy(data, mixed->stereo.data(), bytes);
                        buffer->Unlock();
                        buffer->SetCurrentLength(bytes);
                        sample->SetSampleTime(mixed->pts);
                        sample->SetSampleDuration(
                            static_cast<int64_t>(mixed->frames) * 10'000'000LL /
                            static_cast<int64_t>(kAudioOutputRate));
                        diagnostics_stage_.store(DiagnosticsStage::MuxingAudio, std::memory_order_relaxed);
                        if (muxer_->write_audio(sample.Get())) {
                            audio_written_.fetch_add(1, std::memory_order_relaxed);
                            telemetry_.on_audio_written();
                        } else {
                            report_output_failure(L"audio");
                        }
                    } else {
                        SR_LOG_ERROR(L"Mixed audio buffer lock failed: 0x%08X", hr);
                    }
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
            if (on_ac != last_power_ac_) {
                last_power_ac_ = on_ac;
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

        if (now_ms - last_mem_sample_ms >= 5000) {
            PROCESS_MEMORY_COUNTERS_EX mem{};
            mem.cb = sizeof(mem);
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
                const uint64_t ws_mb = static_cast<uint64_t>(mem.WorkingSetSize / (1024ull * 1024ull));
                const uint64_t private_mb = static_cast<uint64_t>(mem.PrivateUsage / (1024ull * 1024ull));
                if (private_mb >= 700) {
                    SR_LOG_WARN(L"[Perf] High process memory: private=%llu MB, working_set=%llu MB", private_mb, ws_mb);
                } else {
                    SR_LOG_INFO(L"[Perf] Process memory: private=%llu MB, working_set=%llu MB", private_mb, ws_mb);
                }
            }
            if (perf_adapter3) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
                DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal_info{};
                const HRESULT hr_local = perf_adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info);
                const HRESULT hr_nonlocal = perf_adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal_info);
                if (SUCCEEDED(hr_local) && SUCCEEDED(hr_nonlocal)) {
                    const uint64_t local_current_mb = local_info.CurrentUsage / (1024ull * 1024ull);
                    const uint64_t local_budget_mb  = local_info.Budget / (1024ull * 1024ull);
                    const uint64_t shared_current_mb = nonlocal_info.CurrentUsage / (1024ull * 1024ull);
                    const uint64_t shared_budget_mb  = nonlocal_info.Budget / (1024ull * 1024ull);
                    SR_LOG_INFO(
                        L"[Perf] GPU memory: local=%llu/%llu MB, shared=%llu/%llu MB",
                        local_current_mb, local_budget_mb, shared_current_mb, shared_budget_mb);
                }
            }
            last_mem_sample_ms = now_ms;
        }
    }

    // Capture is stopped before this thread is joined; flush the mixer reorder
    // window so the final microphone/system audio samples reach the MP4.
    while (auto mixed = audio_mixer.take(true)) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD bytes = static_cast<DWORD>(mixed->stereo.size() * sizeof(float));
        if (!acquire_audio_sample(bytes, sample, buffer)) continue;
        BYTE* data = nullptr;
        const HRESULT hr = buffer->Lock(&data, nullptr, nullptr);
        if (FAILED(hr) || !data) continue;
        std::memcpy(data, mixed->stereo.data(), bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        sample->SetSampleTime(mixed->pts);
        sample->SetSampleDuration(
            static_cast<int64_t>(mixed->frames) * 10'000'000LL /
            static_cast<int64_t>(kAudioOutputRate));
        if (muxer_->write_audio(sample.Get())) {
            audio_written_.fetch_add(1, std::memory_order_relaxed);
            telemetry_.on_audio_written();
        } else {
            report_output_failure(L"audio");
        }
    }
}

// ---------------------------------------------------------------------------
void SessionController::notify_status(const std::wstring& msg) {
    if (on_status_) on_status_(msg);
}
void SessionController::notify_error(const std::wstring& msg) {
    SR_LOG_ERROR(L"%s", msg.c_str());
    if (on_error_) on_error_(msg);
}

void SessionController::report_output_failure(const wchar_t* stream) {
    if (output_failure_reported_.exchange(true, std::memory_order_acq_rel)) return;
    diagnostics_.write_event(L"event=mux_write_failed stream=" + std::wstring(stream));
    notify_error(L"Recording could not write media data. Stopping; check the saved recording and diagnostics.");
    if ((machine_.is_recording() || machine_.is_paused()) && on_stop_requested_) {
        on_stop_requested_();
    }
}

} // namespace sr
