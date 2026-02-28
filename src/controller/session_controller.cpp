// session_controller.cpp — Wires all engines together into the recording pipeline
// T016: Start -> init engines -> run capture->encode->mux loop -> Stop -> finalize

#include "controller/session_controller.h"
#include "capture/capture_engine.h"
#include "audio/audio_engine.h"
#include "encoder/video_encoder.h"
#include "storage/mux_writer.h"
#include "storage/storage_manager.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <thread>
#include <chrono>

namespace sr {

SessionController::SessionController()
    : capture_(std::make_unique<CaptureEngine>())
    , audio_  (std::make_unique<AudioEngine>())
    , encoder_(std::make_unique<VideoEncoder>())
    , muxer_  (std::make_unique<MuxWriter>())
    , frame_queue_(std::make_unique<FrameQueue>())
    , audio_queue_(std::make_unique<AudioQueue>())
{}

SessionController::~SessionController() {
    if (!machine_.is_idle()) {
        // Force stop if destroyed while recording
        encode_running_.store(false, std::memory_order_release);
        capture_->stop();
        audio_->stop();
        if (encode_thread_.joinable()) encode_thread_.join();
        muxer_->finalize();
    }
}

bool SessionController::initialize(StorageManager* storage,
                                    StatusCallback on_status,
                                    ErrorCallback  on_error)
{
    storage_   = storage;
    on_status_ = std::move(on_status);
    on_error_  = std::move(on_error);

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

    notify_status(L"Starting...");

    // ---------------------------------------------------------------
    // Determine output paths
    // generateFilename() returns the .partial.mp4 path
    // ---------------------------------------------------------------
    current_partial_path_ = storage_ ? storage_->generateFilename() : L"ScreenRec.partial.mp4";
    current_output_path_  = StorageManager::partialToFinal(current_partial_path_);

    // ---------------------------------------------------------------
    // Anchor sync clock
    // ---------------------------------------------------------------
    sync_.start();

    // ---------------------------------------------------------------
    // Initialize CaptureEngine
    // ---------------------------------------------------------------
    if (!capture_->initialize(probe_.d3d_device.Get(),
                               probe_.d3d_context.Get(),
                               frame_queue_.get()))
    {
        notify_error(L"Capture engine initialization failed");
        machine_.transition(SessionEvent::Stop);
        machine_.transition(SessionEvent::Finalized);
        return false;
    }
    capture_->set_sync_anchor_100ns(0); // anchor is always 0; PTS computed from sync_

    // ---------------------------------------------------------------
    // Initialize AudioEngine
    // ---------------------------------------------------------------
    if (!audio_->initialize(audio_queue_.get())) {
        notify_error(L"Audio engine initialization failed (no microphone?)");
        // Non-fatal — continue without audio
        SR_LOG_WARN(L"Continuing without audio");
    } else {
        audio_->set_sync_anchor_100ns(0);
    }

    // ---------------------------------------------------------------
    // Initialize VideoEncoder  (T042: clamp profile for power mode)
    // ---------------------------------------------------------------
    EncoderProfile enc_prof;
    if (has_pending_profile_) {
        enc_prof = pending_profile_;
        SR_LOG_INFO(L"Using custom encoder profile: %ufps, %u bps", enc_prof.fps, enc_prof.bitrate_bps);
    } else {
        enc_prof.fps         = 30;
        enc_prof.bitrate_bps = 8'000'000;
    }
    enc_prof.width  = capture_->width()  ? capture_->width()  : 1920;
    enc_prof.height = capture_->height() ? capture_->height() : 1080;

    // T042: Apply power-mode clamping (battery → 30fps/8Mbps)
    last_power_ac_ = PowerModeDetector::is_on_ac_power();
    enc_prof = PowerModeDetector::clamp_for_power(enc_prof);

    if (!encoder_->initialize(enc_prof,
                               probe_.dxgi_device_manager.Get(),
                               probe_.d3d_device.Get(),
                               probe_.d3d_context.Get()))
    {
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
    mux_cfg.audio_sample_rate      = audio_->sample_rate();
    mux_cfg.audio_channels         = audio_->channels();
    mux_cfg.audio_bits_per_sample  = audio_->bits_per_sample();
    mux_cfg.audio_is_float         = (audio_->bits_per_sample() == 32);

    if (!muxer_->initialize(current_partial_path_, current_output_path_, mux_cfg)) {
        notify_error(L"Mux writer initialization failed");
        machine_.transition(SessionEvent::Stop);
        machine_.transition(SessionEvent::Finalized);
        return false;
    }

    // ---------------------------------------------------------------
    // Start engines
    // ---------------------------------------------------------------
    frames_encoded_.store(0, std::memory_order_relaxed);
    audio_written_.store(0,  std::memory_order_relaxed);
    telemetry_.reset(); // T037: clear counters for new session

    // T038: initialise frame pacer for this session's fps
    pacer_.initialize(enc_prof.fps);

    // T039: register device-lost callback — fires when D3D device is removed
    capture_->set_device_lost_callback([this]() {
        SR_LOG_ERROR(L"[T039] Device-lost event received — auto-stopping recording");
        notify_error(L"\u26A0 Graphics device was reset or removed. Recording stopped.");
        // stop() is safe to call from the WGC frame-arrived callback thread:
        // it uses atomic transitions (same pattern as T031 disk-space auto-stop).
        stop();
    });

    encode_running_.store(true, std::memory_order_release);
    encode_thread_ = std::thread(&SessionController::encode_loop, this);

    if (!capture_->start()) {
        notify_error(L"Capture start failed");
        stop();
        return false;
    }
    audio_->start();

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

    notify_status(L"Recording...");
    SR_LOG_INFO(L"Recording started -> %s", current_output_path_.c_str());
    return true;
}

bool SessionController::stop() {
    bool was_recording = machine_.is_recording() || machine_.is_paused();

    if (!machine_.transition(SessionEvent::Stop)) return false;
    notify_status(L"Stopping...");

    // T031: Stop disk space polling (handles re-entrant call from poll thread)
    if (storage_) storage_->stopDiskSpacePolling();

    // Stop producers first
    capture_->stop();
    audio_->stop();

    // Stop encode loop (it drains remaining frames)
    encode_running_.store(false, std::memory_order_release);
    if (encode_thread_.joinable()) encode_thread_.join();

    // Flush encoder and write remaining samples
    if (was_recording) {
        std::vector<ComPtr<IMFSample>> leftover;
        encoder_->flush(leftover);
        for (auto& s : leftover) {
            muxer_->write_video(s.Get());
        }
    }

    // Finalize mux (rename .partial.mp4 -> .mp4)
    muxer_->finalize();

    machine_.transition(SessionEvent::Finalized);
    notify_status(L"Idle");
    SR_LOG_INFO(L"Recording stopped. Encoded: %u frames, audio pkts: %u",
                frames_encoded_.load(), audio_written_.load());
    return true;
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
    return telemetry_.snapshot(enc_mode, last_power_ac_);
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

    // Interleave video and audio: write video first, then any pending audio
    while (encode_running_.load(std::memory_order_acquire) ||
           !frame_queue_->empty())
    {
        // --- Video ---
        if (auto opt_frame = frame_queue_->try_pop()) {
            auto& frame = *opt_frame;

            // Skip frames while paused
            if (machine_.is_paused()) {
                continue;
            }

            // T038: pace the incoming PTS and decide what to do
            bool   queue_full = (frame_queue_->size() == 0); // already popped — never full here
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
                if (encoder_->encode_frame(last_texture.Get(), dup_pts, dup_sample)) {
                    muxer_->write_video(dup_sample.Get());
                    frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                    telemetry_.on_frame_encoded();
                    telemetry_.on_duplicate_inserted();
                }
            }

            // Cache this texture (ComPtr copy AddRefs it) before encoding
            last_texture = frame.texture;

            // Encode current frame
            ComPtr<IMFSample> encoded;
            if (encoder_->encode_frame(frame.texture.Get(), paced_pts, encoded)) {
                muxer_->write_video(encoded.Get());
                frames_encoded_.fetch_add(1, std::memory_order_relaxed);
                telemetry_.on_frame_encoded();
            }

            have_last_frame = true;
            last_paced_pts  = paced_pts;
        }

        // --- Audio: drain all pending packets ---
        while (auto opt_audio = audio_queue_->try_pop()) {
            auto& audio_pkt = *opt_audio;
            if (machine_.is_paused()) continue;

            // Build IMFSample from PCM buffer
            ComPtr<IMFSample>      sample;
            ComPtr<IMFMediaBuffer> buf;
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(static_cast<DWORD>(audio_pkt.buffer.size()), &buf);

            BYTE* data = nullptr;
            buf->Lock(&data, nullptr, nullptr);
            std::memcpy(data, audio_pkt.buffer.data(), audio_pkt.buffer.size());
            buf->Unlock();
            buf->SetCurrentLength(static_cast<DWORD>(audio_pkt.buffer.size()));

            sample->AddBuffer(buf.Get());
            sample->SetSampleTime(audio_pkt.pts);

            // Duration: frame_count / sample_rate in 100ns
            int64_t dur = static_cast<int64_t>(audio_pkt.frame_count) *
                          10'000'000LL / static_cast<int64_t>(audio_pkt.sample_rate);
            sample->SetSampleDuration(dur);

            muxer_->write_audio(sample.Get());
            audio_written_.fetch_add(1, std::memory_order_relaxed);
            telemetry_.on_audio_written();
        }

        // Brief yield when queue is empty
        if (frame_queue_->empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

} // namespace sr
