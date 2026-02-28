// audio_engine.cpp — WASAPI shared-mode event-driven microphone capture
// T012: Event-driven, MMCSS registered, silence injection for mute

#include "audio/audio_engine.h"
#include "utils/logging.h"

#include <avrt.h>
#include <cstring>
#pragma comment(lib, "avrt.lib")

namespace sr {

bool AudioEngine::initialize(AudioQueue* queue) {
    queue_ = queue;

    // Create device enumerator
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator_)
    );
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"MMDeviceEnumerator failed: 0x%08X", hr);
        return false;
    }

    // Get default communications capture endpoint (microphone)
    hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eCommunications, &device_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"GetDefaultAudioEndpoint(capture) failed: 0x%08X", hr);
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
    SR_LOG_INFO(L"Audio: %u Hz, %u ch, %u-bit", sample_rate_, channels_, bits_per_sample_);

    // Initialize shared mode — 100ms buffer, event-driven
    // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY handles sample rate mismatches
    REFERENCE_TIME buf_duration = 1'000'000; // 100ms in 100ns units
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        buf_duration, 0, mix_fmt, nullptr
    );
    CoTaskMemFree(mix_fmt);

    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IAudioClient::Initialize failed: 0x%08X", hr);
        return false;
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
}

void AudioEngine::capture_loop() {
    // Register with MMCSS for audio-class priority
    DWORD task_idx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_idx);
    if (!mmcss) {
        SR_LOG_WARN(L"AvSetMmThreadCharacteristics failed (non-fatal)");
    }

    while (running_.load(std::memory_order_acquire)) {
        DWORD wait_result = WaitForSingleObject(event_handle_, 200 /*ms timeout*/);
        if (wait_result != WAIT_OBJECT_0) {
            // Timeout or error — loop back and check running_
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
            // PTS from monotonic sample counter (avoids QPC drift vs audio clock)
            int64_t pts = pts_anchor_100ns_ +
                static_cast<int64_t>(sample_count_) * 10'000'000LL /
                static_cast<int64_t>(sample_rate_);

            AudioPacket pkt;
            pkt.frame_count  = frames_available;
            pkt.pts          = pts;
            pkt.sample_rate  = sample_rate_;
            pkt.channels     = channels_;

            uint32_t byte_count = frames_available * block_align_;
            pkt.buffer.resize(byte_count);

            bool silence = muted_.load(std::memory_order_relaxed)
                          || (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (silence) {
                std::memset(pkt.buffer.data(), 0, byte_count);
                pkt.is_silence = true;
            } else {
                std::memcpy(pkt.buffer.data(), data, byte_count);
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
