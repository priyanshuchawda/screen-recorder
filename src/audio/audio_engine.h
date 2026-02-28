#pragma once
// audio_engine.h — WASAPI shared-mode event-driven microphone capture
// T012: AudioEngine: captures PCM, pushes AudioPackets, supports silence injection
// T032: Integrates AudioResampler for native-rate → 48 kHz conversion;
//       IMMNotificationClient handles device invalidation/removal.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <functional>
#include "utils/render_frame.h"
#include "utils/bounded_queue.h"
#include "audio/audio_resampler.h"

namespace sr {

using Microsoft::WRL::ComPtr;

// 16-slot audio queue (audio runs at ~10ms packets, needs more headroom than video)
using AudioQueue = BoundedQueue<AudioPacket, 16>;

// Device-invalidation callback type — called when the audio device is removed
// or invalidated (e.g., USB mic unplugged). If set, engine signals the caller
// so recording can gracefully stop or switch to the next default device.
using AudioDeviceInvalidCallback = std::function<void()>;

// ─── IMMNotificationClient implementation ────────────────────────────────────
// Monitors audio endpoint changes and fires the invalidation callback.
// Ref-counted via atomic to satisfy COM requirements.
class AudioDeviceNotifier : public IMMNotificationClient {
public:
    void setup(std::wstring endpoint_id, AudioDeviceInvalidCallback cb) {
        endpoint_id_ = std::move(endpoint_id);
        callback_    = std::move(cb);
        fired_.store(false, std::memory_order_relaxed);
    }

    // IUnknown — lightweight ref counting (AudioEngine owns the lifetime)
    ULONG   STDMETHODCALLTYPE AddRef()  override { return ++ref_; }
    ULONG   STDMETHODCALLTYPE Release() override { return --ref_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (endpoint_id_ == id &&
            (state == DEVICE_STATE_DISABLED ||
             state == DEVICE_STATE_NOTPRESENT ||
             state == DEVICE_STATE_UNPLUGGED)) { fire(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (endpoint_id_ == id) fire(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eCapture && role == eCommunications) fire(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void fire() {
        bool expected = false;
        if (fired_.compare_exchange_strong(expected, true) && callback_) callback_();
    }
    std::wstring               endpoint_id_;
    AudioDeviceInvalidCallback callback_;
    std::atomic<ULONG>         ref_  { 1 };
    std::atomic<bool>          fired_{ false };
};

// ─── AudioEngine ─────────────────────────────────────────────────────────────
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

    // Register device-invalidation callback (T032).
    // Fired once if the active capture device is removed or invalidated.
    void set_device_invalid_callback(AudioDeviceInvalidCallback cb) {
        device_invalid_cb_ = std::move(cb);
    }

    // Audio format — always returns 48 kHz (resampled if device differs)
    uint32_t sample_rate()     const {
        return resampler_.is_passthrough() ? sample_rate_ : resampler_.output_rate();
    }
    uint16_t channels()        const { return channels_; }
    uint32_t bits_per_sample() const { return bits_per_sample_; }

    // Native device rate (before resampling) — for diagnostics
    uint32_t native_sample_rate() const { return sample_rate_; }

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
    uint32_t    sample_rate_    = 48000;  // native device rate
    uint16_t    channels_       = 2;
    uint32_t    bits_per_sample_= 16;
    uint32_t    block_align_    = 4;

    // T032: MF Resampler for native-rate → 48 kHz conversion
    AudioResampler             resampler_;

    // T032: device-invalidation notification
    AudioDeviceInvalidCallback device_invalid_cb_;
    AudioDeviceNotifier        notifier_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> muted_  { false };
    std::thread       thread_;

    int64_t  pts_anchor_100ns_ = 0;   // 100ns epoch offset
    int64_t  sample_count_     = 0;   // monotonic sample counter for PTS
};

} // namespace sr
