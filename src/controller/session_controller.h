#pragma once
// session_controller.h — Wires all engines together; driven by SessionMachine events
// T016: Start initializes all engines, runs capture->preprocess->encode->mux pipeline,
//       Stop finalizes and renames file, Pause/Resume propagates to sync manager.

#include <windows.h>
#include <atomic>
#include <thread>
#include <functional>
#include <string>
#include "controller/session_machine.h"
#include "encoder/encoder_probe.h"
#include "sync/sync_manager.h"
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"
#include "capture/capture_engine.h"  // for FrameQueue typedef
#include "audio/audio_engine.h"     // for AudioQueue typedef

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

class SessionController {
public:
    SessionController();
    ~SessionController();

    // One-time setup; must be called before any Start
    bool initialize(StorageManager* storage,
                    StatusCallback on_status = nullptr,
                    ErrorCallback  on_error  = nullptr);

    // Start recording — transitions state machine Idle->Recording
    bool start();

    // Stop recording — transitions Recording/Paused->Stopping->Idle
    bool stop();

    // Pause — Recording->Paused
    bool pause();

    // Resume — Paused->Recording
    bool resume();

    // Override encoder profile (fps/bitrate/resolution) — must be called before start()
    void set_encoder_profile(const EncoderProfile& profile) { pending_profile_ = profile; has_pending_profile_ = true; }

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

    std::wstring output_path() const { return current_output_path_; }

private:
    // Encode/mux pipeline — runs on encode_thread_
    void encode_loop();

    // Shared state
    SessionMachine  machine_;
    SyncManager     sync_;
    ProbeResult     probe_;

    // Engines
    std::unique_ptr<CaptureEngine> capture_;
    std::unique_ptr<AudioEngine>   audio_;
    std::unique_ptr<VideoEncoder>  encoder_;
    std::unique_ptr<MuxWriter>     muxer_;
    StorageManager*                storage_ = nullptr;

    // Queues shared between capture/audio producers and encode consumer
    std::unique_ptr<FrameQueue> frame_queue_;
    std::unique_ptr<AudioQueue> audio_queue_;

    // Encode thread
    std::thread       encode_thread_;
    std::atomic<bool> encode_running_{ false };

    // Counters
    std::atomic<uint32_t> frames_encoded_{ 0 };
    std::atomic<uint32_t> audio_written_ { 0 };

    // Output path for current recording
    std::wstring current_output_path_;
    std::wstring current_partial_path_;

    // Optional encoder profile override (set via set_encoder_profile before start)
    EncoderProfile pending_profile_;
    bool           has_pending_profile_ = false;

    // Callbacks
    StatusCallback on_status_;
    ErrorCallback  on_error_;

    void notify_status(const std::wstring& msg);
    void notify_error (const std::wstring& msg);
};

} // namespace sr
