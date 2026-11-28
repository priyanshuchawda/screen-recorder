// audio_engine.cpp — WASAPI shared-mode event-driven audio capture
// T012: Event-driven, MMCSS registered, silence injection for mute
// T032: MF Resampler integration for native-rate → 48 kHz conversion;
//       IMMNotificationClient for device invalidation/removal detection.
// Supports Microphone (eCapture) and Loopback (eRender) capture modes.

#include "audio/audio_engine.h"
#include "sync/sync_manager.h"
#include "utils/logging.h"

#include <avrt.h>
#include <audiopolicy.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <exception>
#include <system_error>
#pragma comment(lib, "avrt.lib")

namespace sr {

bool AudioEngine::initialize(AudioQueue* queue, AudioCaptureMode mode) {
    shutdown();
    discontinuities_.store(0, std::memory_order_relaxed);
    estimated_missing_duration_100ns_.store(0, std::memory_order_relaxed);
    queue_ = queue;
    mode_  = mode;
    bool initialized = false;
    try {
        initialized = queue_ && initialize_impl();
    } catch (const std::exception& e) {
        SR_LOG_ERROR(L"AudioEngine initialization exception: %S", e.what());
    } catch (...) {
        SR_LOG_ERROR(L"AudioEngine initialization failed with an unknown exception");
    }
    if (!initialized) {
        shutdown();
        return false;
    }
    initialized_ = true;
    return true;
}

bool AudioEngine::initialize_impl() {

    const bool is_loopback = (mode_ == AudioCaptureMode::Loopback);
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
    is_float_ = mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mix_fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
        is_float_ = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    if (bits_per_sample_ != 16 && !(bits_per_sample_ == 32 && is_float_)) {
        SR_LOG_ERROR(L"[%s] Unsupported WASAPI format: %u-bit %s", mode_name,
                     bits_per_sample_, is_float_ ? L"float" : L"integer");
        CoTaskMemFree(mix_fmt);
        return false;
    }
    block_align_     = mix_fmt->nBlockAlign;
    SR_LOG_INFO(L"[%s] Audio device: %u Hz, %u ch, %u-bit", mode_name,
                sample_rate_, channels_, bits_per_sample_);

    // T032: Initialize MF Resampler if device rate differs from 48 kHz
    if (!resampler_.initialize(sample_rate_, static_cast<uint16_t>(channels_),
                                bits_per_sample_, 48000)) {
        SR_LOG_ERROR(L"[%s] AudioResampler failed; cannot maintain synchronized 48 kHz mix", mode_name);
        CoTaskMemFree(mix_fmt);
        return false;
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
    hr = enumerator_->RegisterEndpointNotificationCallback(&notifier_);
    notifier_registered_ = SUCCEEDED(hr);
    if (FAILED(hr)) {
        SR_LOG_WARN(L"[%s] RegisterEndpointNotificationCallback failed: 0x%08X",
                    mode_name, hr);
    }
    }

    SR_LOG_INFO(L"[%s] AudioEngine initialized", mode_name);

    return true;
}

bool AudioEngine::start() {
    if (!initialized_ || !audio_client_ || !capture_client_ || !event_handle_) {
        SR_LOG_ERROR(L"AudioEngine::start rejected: endpoint initialization did not complete");
        return false;
    }
    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        SR_LOG_ERROR(L"AudioEngine::start rejected: capture is already active");
        return false;
    }
    sample_count_ = 0;
    queue_drops_.store(0, std::memory_order_relaxed);
    resampler_failures_.store(0, std::memory_order_relaxed);
    mic_level_envelope_ = 0.0f;
    mic_gain_linear_ = 1.0f;
    LARGE_INTEGER qpc_now{}, qpc_frequency{};
    QueryPerformanceCounter(&qpc_now);
    QueryPerformanceFrequency(&qpc_frequency);
    if (qpc_frequency.QuadPart > 0) {
        const int64_t absolute_100ns = static_cast<int64_t>(
            static_cast<double>(qpc_now.QuadPart) * 10'000'000.0 /
            static_cast<double>(qpc_frequency.QuadPart));
        start_pts_100ns_ = (std::max)(int64_t{0}, absolute_100ns - pts_anchor_100ns_);
    }
    running_.store(true, std::memory_order_release);

    HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IAudioClient::Start failed: 0x%08X", hr);
        running_.store(false, std::memory_order_release);
        return false;
    }

    try {
        thread_ = std::thread(&AudioEngine::capture_loop, this);
    } catch (const std::system_error& e) {
        running_.store(false, std::memory_order_release);
        audio_client_->Stop();
        SR_LOG_ERROR(L"Audio capture thread creation failed: %S", e.what());
        return false;
    }
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
    if (audio_client_) audio_client_->Stop();
    if (event_handle_) {
        CloseHandle(event_handle_);
        event_handle_ = nullptr;
    }
    // Unregister device notification (T032)
    if (enumerator_ && notifier_registered_) {
        enumerator_->UnregisterEndpointNotificationCallback(&notifier_);
        notifier_registered_ = false;
    }
    // Drain the resampler tail into the still-live packet queue before the
    // encode worker performs its final audio-mixer flush.
    if (initialized_ && !resampler_.is_passthrough()) {
        std::vector<uint8_t> tail;
        int64_t tail_pts = 0;
        const bool flush_ok = resampler_.flush(tail, &tail_pts);
        if (!flush_ok) {
            const uint32_t failures = resampler_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
            SR_LOG_ERROR(L"[%s] Resampler final drain failed (total failures=%u)",
                         mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone",
                         failures);
        }
        if (!tail.empty() && queue_) {
            AudioPacket packet;
            packet.frame_count = block_align_ > 0
                ? static_cast<uint32_t>(tail.size() / block_align_) : 0;
            packet.pts = tail_pts;
            packet.sample_rate = resampler_.output_rate();
            packet.channels = channels_;
            packet.bits_per_sample = static_cast<uint16_t>(bits_per_sample_);
            packet.is_float = is_float_;
            packet.buffer = std::move(tail);
            if (!queue_->try_push(std::move(packet))) {
                const uint32_t drops = queue_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
                SR_LOG_ERROR(L"[%s] Audio queue full while enqueuing resampler tail (total drops=%u)",
                             mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone",
                             drops);
            }
        }
    }
    resampler_.shutdown();
}

void AudioEngine::shutdown() {
    stop();
    if (event_handle_) {
        CloseHandle(event_handle_);
        event_handle_ = nullptr;
    }
    capture_client_.Reset();
    audio_client_.Reset();
    device_.Reset();
    enumerator_.Reset();
    queue_ = nullptr;
    initialized_ = false;
    sample_rate_ = 48'000;
    channels_ = 2;
    bits_per_sample_ = 16;
    is_float_ = false;
    block_align_ = 4;
}

void AudioEngine::capture_loop() {
    // Register with MMCSS for audio-class priority
    DWORD task_idx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_idx);
    if (!mmcss) {
        SR_LOG_WARN(L"AvSetMmThreadCharacteristics failed (non-fatal)");
    }

    // Level the microphone gently, but never gate by amplitude: quiet words,
    // breaths and word beginnings must survive even when the room is quiet.
    const bool apply_mic_leveling = (mode_ == AudioCaptureMode::Microphone);
    constexpr float kMicTargetRms = 0.126f;            // about -18 dBFS
    constexpr float kMicMinGain = 0.25f;
    constexpr float kMicMaxGain = 4.0f;                // +12 dB maximum boost
    if (apply_mic_leveling) {
        SR_LOG_INFO(L"Microphone auto-level active (no hard gate): target -18 dBFS, max boost +12 dB");
    }

    std::vector<uint8_t> resampled_buf;
    bool logged_timestamp_error = false;
    int64_t last_packet_end_pts = -1;
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

        UINT64 qpc_position = 0;
        while (SUCCEEDED(capture_client_->GetBuffer(
                &data, &frames_available, &flags, nullptr, &qpc_position))
               && frames_available > 0)
        {
            int64_t pts = last_packet_end_pts >= 0
                ? last_packet_end_pts
                : start_pts_100ns_ + static_cast<int64_t>(sample_count_) *
                    10'000'000LL / static_cast<int64_t>(sample_rate_);
            bool timestamp_in_pause = sync_manager_ && sync_manager_->is_paused();
            const bool timestamp_valid = qpc_position != 0 &&
                !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
            bool timestamp_mapped = false;
            if (timestamp_valid) {
                if (sync_manager_) {
                    const int64_t packet_duration = static_cast<int64_t>(frames_available) *
                        10'000'000LL / static_cast<int64_t>(sample_rate_);
                    timestamp_mapped = sync_manager_->try_map_qpc_100ns(
                        static_cast<int64_t>(qpc_position), packet_duration, pts);
                    timestamp_in_pause = !timestamp_mapped;
                } else {
                    pts = static_cast<int64_t>(qpc_position) - pts_anchor_100ns_;
                    timestamp_mapped = true;
                }
            } else if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && !logged_timestamp_error) {
                logged_timestamp_error = true;
                SR_LOG_WARN(L"[%s] WASAPI timestamp uncertain; using sample clock",
                            mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone");
            }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                const uint64_t total = discontinuities_.fetch_add(1, std::memory_order_relaxed) + 1;
                int64_t estimated_gap_100ns = 0;
                if (timestamp_mapped && last_packet_end_pts >= 0 && pts > last_packet_end_pts) {
                    estimated_gap_100ns = pts - last_packet_end_pts;
                    estimated_missing_duration_100ns_.fetch_add(
                        static_cast<uint64_t>(estimated_gap_100ns), std::memory_order_relaxed);
                }
                if (total == 1 || (total % 100) == 0) {
                    SR_LOG_WARN(L"[%s] WASAPI discontinuity count=%llu estimated_gap_ms=%lld",
                        mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone",
                        static_cast<unsigned long long>(total),
                        static_cast<long long>(estimated_gap_100ns / 10'000));
                }
            }
            if (timestamp_in_pause) {
                capture_client_->ReleaseBuffer(frames_available);
                sample_count_ += static_cast<int64_t>(frames_available);
                continue;
            }
            pts = (std::max)(int64_t{0}, pts);
            last_packet_end_pts = pts + static_cast<int64_t>(frames_available) *
                10'000'000LL / static_cast<int64_t>(sample_rate_);

            uint32_t byte_count = frames_available * block_align_;

            bool silence = muted_.load(std::memory_order_relaxed)
                          || (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            float mic_rms = 0.0f;

            // Measure microphone level for automatic gain control. Do not
            // replace low-level input with silence; this used to clip quiet
            // speech and word onsets at the noise-gate threshold.
            if (apply_mic_leveling && !silence && data && byte_count > 0) {
                if (is_float_) {
                    const float* samples = reinterpret_cast<const float*>(data);
                    const size_t count = byte_count / sizeof(float);
                    double sum_sq = 0.0;
                    for (size_t i = 0; i < count; ++i) {
                        sum_sq += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
                    }
                    mic_rms = count > 0 ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count))) : 0.0f;
                } else if (bits_per_sample_ == 16) {
                    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                    const size_t count = byte_count / sizeof(int16_t);
                    int64_t sum_sq = 0;
                    for (size_t i = 0; i < count; ++i) {
                        sum_sq += static_cast<int64_t>(samples[i]) * static_cast<int64_t>(samples[i]);
                    }
                    double rms = count > 0 ? std::sqrt(static_cast<double>(sum_sq) / static_cast<double>(count)) : 0.0;
                    mic_rms = static_cast<float>(rms / 32768.0);
                }
            }

            float packet_mic_gain = 1.0f;
            if (apply_mic_leveling && !silence && mic_rms > 0.0f) {
                // Smooth the level estimate to prevent gain pumping between
                // individual 10 ms WASAPI packets. Reduce gain quickly to
                // protect loud speech; restore it more gradually.
                const float level_alpha = mic_rms > mic_level_envelope_ ? 0.25f : 0.06f;
                mic_level_envelope_ += level_alpha * (mic_rms - mic_level_envelope_);
                const float safe_level = (mic_level_envelope_ > 0.0001f)
                    ? mic_level_envelope_ : 0.0001f;
                const float target_gain = std::clamp(
                    kMicTargetRms / safe_level, kMicMinGain, kMicMaxGain);
                const float gain_alpha = target_gain < mic_gain_linear_ ? 0.5f : 0.04f;
                mic_gain_linear_ += gain_alpha * (target_gain - mic_gain_linear_);
                packet_mic_gain = mic_gain_linear_;
            }

            // Convert all non-canonical rates, including silence and mute,
            // through the same stateful resampler so packet duration and
            // filter state remain continuous.
            const uint8_t* pkt_data = nullptr;
            uint32_t       pkt_bytes = byte_count;
            int64_t packet_pts = pts;

            if (!resampler_.is_passthrough()) {
                resampled_buf.clear();
                const bool resample_ok = resampler_.process(
                    data, byte_count, resampled_buf, silence, pts, &packet_pts);
                if (!resample_ok) {
                    const uint32_t failures = resampler_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (failures == 1 || (failures % 100) == 0) {
                        SR_LOG_ERROR(L"[%s] Audio resampling failed (total failures=%u)",
                                     mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone",
                                     failures);
                    }
                }
                pkt_data  = resampled_buf.data();
                pkt_bytes = static_cast<uint32_t>(resampled_buf.size());
            } else {
                pkt_data = data;
            }

            // A resampler can legitimately buffer its first input while its
            // filter warms up. Keep its timeline state and wait for real output
            // instead of queuing a zero-frame packet.
            if (pkt_bytes == 0) {
                capture_client_->ReleaseBuffer(frames_available);
                sample_count_ += static_cast<int64_t>(frames_available);
                continue;
            }

            uint32_t resampled_frames = (block_align_ > 0)
                ? (pkt_bytes / block_align_)
                : frames_available;

            AudioPacket pkt;
            pkt.frame_count  = resampled_frames;
            pkt.pts          = packet_pts;
            pkt.sample_rate  = sample_rate();
            pkt.channels     = channels_;
            pkt.bits_per_sample = static_cast<uint16_t>(bits_per_sample_);
            pkt.is_float = is_float_;

            pkt.buffer.resize(pkt_bytes);
            if (silence) {
                std::memset(pkt.buffer.data(), 0, pkt_bytes);
                pkt.is_silence = true;
            } else {
                std::memcpy(pkt.buffer.data(), pkt_data, pkt_bytes);
                pkt.is_silence = false;

                // Apply microphone-only automatic level control after any
                // resampling. A soft peak limiter avoids hard clipping when
                // the adaptive gain raises a transient.
                if (apply_mic_leveling) {
                    auto limit_sample = [](float value) {
                        constexpr float limit_start = 0.80f;
                        constexpr float limit_knee = 0.09f; // ceiling about -1 dBFS
                        const float magnitude = std::fabs(value);
                        if (magnitude <= limit_start) return value;
                        const float limited = limit_start + limit_knee *
                            (1.0f - std::exp(-(magnitude - limit_start) / limit_knee));
                        return std::copysign((limited > 0.999f) ? 0.999f : limited, value);
                    };
                    if (is_float_) {
                        auto* samples = reinterpret_cast<float*>(pkt.buffer.data());
                        const size_t count = pkt.buffer.size() / sizeof(float);
                        for (size_t i = 0; i < count; ++i) {
                            samples[i] = limit_sample(samples[i] * packet_mic_gain);
                        }
                    } else if (bits_per_sample_ == 16) {
                        auto* samples = reinterpret_cast<int16_t*>(pkt.buffer.data());
                        const size_t count = pkt.buffer.size() / sizeof(int16_t);
                        for (size_t i = 0; i < count; ++i) {
                            const float normalized = static_cast<float>(samples[i]) / 32768.0f;
                            const float limited = limit_sample(normalized * packet_mic_gain);
                            const long scaled = std::lround(limited * 32767.0f);
                            samples[i] = static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
                        }
                    }
                }
            }

            capture_client_->ReleaseBuffer(frames_available);
            sample_count_ += static_cast<int64_t>(frames_available);

            if (queue_) {
                if (!queue_->try_push(std::move(pkt))) {
                    const uint32_t drops = queue_drops_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (drops == 1 || (drops % 100) == 0) {
                        SR_LOG_WARN(L"[%s] Audio packet queue full; packets dropped=%u",
                                    mode_ == AudioCaptureMode::Loopback ? L"Loopback" : L"Microphone",
                                    drops);
                    }
                }
            }
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace sr
