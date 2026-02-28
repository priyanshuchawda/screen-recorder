#pragma once
// audio_engine.h — WASAPI shared-mode event-driven microphone capture
// T012: AudioEngine: captures PCM, pushes AudioPackets, supports silence injection

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"

namespace sr {

using Microsoft::WRL::ComPtr;

// 16-slot audio queue (audio runs at ~10ms packets, needs more headroom than video)
using AudioQueue = BoundedQueue<AudioPacket, 16>;

class AudioEngine {
public:
    AudioEngine()  = default;
    ~AudioEngine() { stop(); }

    // Attach an output queue and configure the WASAPI capture device.
    // Must be called before start().
    bool initialize(AudioQueue* queue);

    // Start the capture thread
    bool start();

    // Signal stop and join the capture thread
    void stop();

    // Toggle mute: when muted, zeroed PCM is pushed instead of real audio
    void set_muted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    bool is_muted()    const   { return muted_.load(std::memory_order_relaxed); }

    // Audio format (valid after initialize())
    uint32_t sample_rate()    const { return sample_rate_; }
    uint16_t channels()       const { return channels_; }
    uint32_t bits_per_sample()const { return bits_per_sample_; }

    // Set QPC anchor for PTS calculation (call just before start())
    void set_sync_anchor_100ns(int64_t anchor) { pts_anchor_100ns_ = anchor; }

private:
    void capture_loop();

    ComPtr<IMMDeviceEnumerator>  enumerator_;
    ComPtr<IMMDevice>            device_;
    ComPtr<IAudioClient>         audio_client_;
    ComPtr<IAudioCaptureClient>  capture_client_;
    HANDLE                       event_handle_ = nullptr;

    AudioQueue* queue_          = nullptr;
    uint32_t    sample_rate_    = 48000;
    uint16_t    channels_       = 2;
    uint32_t    bits_per_sample_= 16;
    uint32_t    block_align_    = 4;

    std::atomic<bool> running_{ false };
    std::atomic<bool> muted_  { false };
    std::thread       thread_;

    int64_t  pts_anchor_100ns_ = 0;   // 100ns epoch offset
    int64_t  sample_count_     = 0;   // monotonic sample counter for PTS
};

} // namespace sr
