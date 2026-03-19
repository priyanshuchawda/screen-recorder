// audio_engine.cpp — WASAPI shared-mode event-driven audio capture
// T012: Event-driven, MMCSS registered, silence injection for mute
// T032: MF Resampler integration for native-rate → 48 kHz conversion;
//       IMMNotificationClient for device invalidation/removal detection.
// Supports Microphone (eCapture) and Loopback (eRender) capture modes.

#include "audio/audio_engine.h"
#include "utils/logging.h"

#include <avrt.h>
#include <audiopolicy.h>
#include <cstring>
#include <cmath>
#pragma comment(lib, "avrt.lib")

namespace sr {

bool AudioEngine::initialize(AudioQueue* queue, AudioCaptureMode mode) {
    queue_ = queue;
    mode_  = mode;

    const bool is_loopback = (mode == AudioCaptureMode::Loopback);
    const wchar_t* mode_name = is_loopback ? L"Loopback" : L"Microphone";

    // Create device enumerator
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator_)
    );
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"[%s] MMDeviceEnumerator failed: 0x%08X", mode_name, hr);
        return false;
    }

    // Both use eConsole role:
    //   Microphone: eCapture + eConsole (NOT eCommunications — that triggers Windows ducking)
    //   Loopback:   eRender  + eConsole (captures what the speakers play)
    EDataFlow flow = is_loopback ? eRender : eCapture;
    ERole     role = eConsole;
    hr = enumerator_->GetDefaultAudioEndpoint(flow, role, &device_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"[%s] GetDefaultAudioEndpoint failed: 0x%08X", mode_name, hr);
        return false;
    }

    // Activate IAudioClient
    hr = device_->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(audio_client_.GetAddressOf())
    );
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IAudioClient activate failed: 0x%08X", hr);
        return false;
    }

    // Get the mix format (device native format)
    WAVEFORMATEX* mix_fmt = nullptr;
    hr = audio_client_->GetMixFormat(&mix_fmt);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"GetMixFormat failed: 0x%08X", hr);
        return false;
    }

    sample_rate_     = mix_fmt->nSamplesPerSec;
    channels_        = mix_fmt->nChannels;
    bits_per_sample_ = mix_fmt->wBitsPerSample;
    block_align_     = mix_fmt->nBlockAlign;
    SR_LOG_INFO(L"[%s] Audio device: %u Hz, %u ch, %u-bit", mode_name,
                sample_rate_, channels_, bits_per_sample_);

    // T032: Initialize MF Resampler if device rate differs from 48 kHz
    if (!resampler_.initialize(sample_rate_, static_cast<uint16_t>(channels_),
                                bits_per_sample_, 48000)) {
        SR_LOG_WARN(L"AudioResampler init failed — continuing at native rate %u Hz", sample_rate_);
    } else if (!resampler_.is_passthrough()) {
        SR_LOG_INFO(L"AudioResampler active: %u Hz -> 48000 Hz", sample_rate_);
    }

    // Initialize shared mode — 100ms buffer, event-driven
    // Loopback requires AUDCLNT_STREAMFLAGS_LOOPBACK on the render endpoint
    REFERENCE_TIME buf_duration = 1'000'000; // 100ms in 100ns units
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (is_loopback) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        buf_duration, 0, mix_fmt, nullptr
    );
    CoTaskMemFree(mix_fmt);

    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IAudioClient::Initialize failed: 0x%08X", hr);
        return false;
    }

    // Opt-out of Windows communications ducking.
    // When any app opens an eCommunications stream, Windows can auto-duck
    // all other audio. We disable this by setting DuckingPreference.
    {
        ComPtr<IAudioSessionControl> session_ctrl;
        if (SUCCEEDED(audio_client_->GetService(IID_PPV_ARGS(&session_ctrl)))) {
            ComPtr<IAudioSessionControl2> session_ctrl2;
            if (SUCCEEDED(session_ctrl.As(&session_ctrl2))) {
                session_ctrl2->SetDuckingPreference(TRUE);
                SR_LOG_INFO(L"[%s] Ducking opt-out set", mode_name);
            }
        }
    }

    // Create event handle + register it
    event_handle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle_) {
        SR_LOG_ERROR(L"CreateEvent for audio failed");
        return false;
    }
    hr = audio_client_->SetEventHandle(event_handle_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SetEventHandle failed: 0x%08X", hr);
        return false;
    }

    // Get capture client
    hr = audio_client_->GetService(IID_PPV_ARGS(&capture_client_));
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"GetService(IAudioCaptureClient) failed: 0x%08X", hr);
        return false;
    }

    // T032: Register IMMNotificationClient for device invalidation
    {
        LPWSTR dev_id = nullptr;
        if (SUCCEEDED(device_->GetId(&dev_id)) && dev_id) {
            notifier_.setup(dev_id, device_invalid_cb_, flow);
            CoTaskMemFree(dev_id);
        }
        enumerator_->RegisterEndpointNotificationCallback(&notifier_);
    }

    SR_LOG_INFO(L"[%s] AudioEngine initialized", mode_name);

    return true;
}

bool AudioEngine::start() {
    sample_count_ = 0;
    running_.store(true, std::memory_order_release);

    HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IAudioClient::Start failed: 0x%08X", hr);
        running_.store(false, std::memory_order_release);
        return false;
    }

    thread_ = std::thread(&AudioEngine::capture_loop, this);
    return true;
}

void AudioEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (event_handle_) {
        SetEvent(event_handle_); // Wake the waiting thread
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (audio_client_) {
        audio_client_->Stop();
    }
    if (event_handle_) {
        CloseHandle(event_handle_);
        event_handle_ = nullptr;
    }
    // Unregister device notification (T032)
    if (enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(&notifier_);
    }
    // Flush any buffered resampler tail
    if (!resampler_.is_passthrough()) {
        std::vector<uint8_t> tail;
        resampler_.flush(tail);  // discard — no queue after stop
    }
}

void AudioEngine::capture_loop() {
    // Register with MMCSS for audio-class priority
    DWORD task_idx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_idx);
    if (!mmcss) {
        SR_LOG_WARN(L"AvSetMmThreadCharacteristics failed (non-fatal)");
    }

    // Noise gate: for microphone mode, zero audio blocks whose RMS
    // is below this threshold. This eliminates constant mic hiss/static.
    // Threshold is tuned for typical laptop/USB mics.
    const bool apply_noise_gate = (mode_ == AudioCaptureMode::Microphone);
    // For 32-bit float: ~0.003 (-50 dBFS). For 16-bit int: ~100/32768.
    constexpr float kNoiseGateThresholdFloat = 0.003f;
    constexpr int32_t kNoiseGateThresholdInt16 = 100; // ~-50 dBFS

    std::vector<uint8_t> resampled_buf;
    while (running_.load(std::memory_order_acquire)) {
        DWORD wait_result = WaitForSingleObject(event_handle_, 200 /*ms timeout*/);
        if (wait_result != WAIT_OBJECT_0) {
            continue;
        }
        if (!running_.load(std::memory_order_acquire)) break;

        // Drain all available packets
        UINT32 frames_available = 0;
        BYTE*  data             = nullptr;
        DWORD  flags            = 0;

        while (SUCCEEDED(capture_client_->GetBuffer(
                &data, &frames_available, &flags, nullptr, nullptr))
               && frames_available > 0)
        {
            int64_t pts = pts_anchor_100ns_ +
                static_cast<int64_t>(sample_count_) * 10'000'000LL /
                static_cast<int64_t>(sample_rate_);

            uint32_t byte_count = frames_available * block_align_;

            bool silence = muted_.load(std::memory_order_relaxed)
                          || (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // Noise gate for microphone: compute RMS and gate if below threshold
            if (apply_noise_gate && !silence && data && byte_count > 0) {
                if (bits_per_sample_ == 32) {
                    const float* samples = reinterpret_cast<const float*>(data);
                    const size_t count = byte_count / sizeof(float);
                    double sum_sq = 0.0;
                    for (size_t i = 0; i < count; ++i) {
                        sum_sq += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
                    }
                    float rms = count > 0 ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count))) : 0.0f;
                    if (rms < kNoiseGateThresholdFloat) {
                        silence = true;
                    }
                } else if (bits_per_sample_ == 16) {
                    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                    const size_t count = byte_count / sizeof(int16_t);
                    int64_t sum_sq = 0;
                    for (size_t i = 0; i < count; ++i) {
                        sum_sq += static_cast<int64_t>(samples[i]) * static_cast<int64_t>(samples[i]);
                    }
                    double rms = count > 0 ? std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(count)) : 0.0;
                    if (rms < static_cast<double>(kNoiseGateThresholdInt16)) {
                        silence = true;
                    }
                }
            }

            // T032: resample if native rate != 48 kHz
            const uint8_t* pkt_data = nullptr;
            uint32_t       pkt_bytes = byte_count;

            if (!resampler_.is_passthrough() && !silence) {
                resampled_buf.clear();
                resampler_.process(data, byte_count, resampled_buf);
                pkt_data  = resampled_buf.data();
                pkt_bytes = static_cast<uint32_t>(resampled_buf.size());
            } else {
                pkt_data = data;
            }

            uint32_t resampled_frames = (block_align_ > 0)
                ? (pkt_bytes / block_align_)
                : frames_available;

            AudioPacket pkt;
            pkt.frame_count  = resampled_frames;
            pkt.pts          = pts;
            pkt.sample_rate  = sample_rate();
            pkt.channels     = channels_;

            pkt.buffer.resize(pkt_bytes);
            if (silence) {
                std::memset(pkt.buffer.data(), 0, pkt_bytes);
                pkt.is_silence = true;
            } else {
                std::memcpy(pkt.buffer.data(), pkt_data, pkt_bytes);
                pkt.is_silence = false;
            }

            capture_client_->ReleaseBuffer(frames_available);
            sample_count_ += static_cast<int64_t>(frames_available);

            if (queue_) {
                queue_->try_push(std::move(pkt));
            }
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace sr
