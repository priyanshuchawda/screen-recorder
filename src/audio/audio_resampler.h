#pragma once
// audio_resampler.h — checked Media Foundation PCM sample-rate conversion.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "utils/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

namespace sr {

using Microsoft::WRL::ComPtr;

namespace audio_resampler_detail {

template <typename Submit, typename Drain>
HRESULT submit_with_drain_retry(Submit&& submit, Drain&& drain,
                                unsigned max_attempts) {
    if (max_attempts == 0) return E_INVALIDARG;
    for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
        const HRESULT hr = submit();
        if (hr != MF_E_NOTACCEPTING) return hr;
        if (attempt + 1 == max_attempts) return hr;
        if (!drain()) return E_FAIL;
    }
    return E_FAIL;
}

} // namespace audio_resampler_detail

class AudioResampler {
public:
    AudioResampler() = default;
    ~AudioResampler() { shutdown(); }

    bool initialize(uint32_t input_rate, uint16_t channels, uint32_t bits,
                    uint32_t output_rate = 48'000) {
        shutdown();
        if (input_rate == 0 || output_rate == 0 || channels == 0 ||
            (bits != 16 && bits != 32)) {
            SR_LOG_ERROR(L"AudioResampler: invalid format %u Hz, %u ch, %u-bit",
                         input_rate, channels, bits);
            return false;
        }

        in_rate_ = input_rate;
        out_rate_ = output_rate;
        channels_ = channels;
        bits_ = bits;
        input_block_align_ = static_cast<uint32_t>(channels) * (bits / 8);
        output_block_align_ = input_block_align_;
        if (input_rate == output_rate) {
            passthrough_ = true;
            SR_LOG_INFO(L"AudioResampler: same rate (%u Hz) — passthrough mode", input_rate);
            return true;
        }

        HRESULT hr = CoCreateInstance(CLSID_CResamplerMediaObject, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: CoCreateInstance failed: 0x%08X", hr);
            return false;
        }

        ComPtr<IMFMediaType> input_type;
        hr = MFCreateMediaType(&input_type);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: input media type creation failed: 0x%08X", hr);
            shutdown();
            return false;
        }
        hr = set_pcm_type(input_type.Get(), input_rate, channels, bits);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: configuring input media type failed: 0x%08X", hr);
            shutdown();
            return false;
        }
        hr = mft_->SetInputType(0, input_type.Get(), 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetInputType failed: 0x%08X", hr);
            shutdown();
            return false;
        }

        ComPtr<IMFMediaType> output_type;
        hr = MFCreateMediaType(&output_type);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: output media type creation failed: 0x%08X", hr);
            shutdown();
            return false;
        }
        hr = set_pcm_type(output_type.Get(), output_rate, channels, bits);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: configuring output media type failed: 0x%08X", hr);
            shutdown();
            return false;
        }
        hr = mft_->SetOutputType(0, output_type.Get(), 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetOutputType failed: 0x%08X", hr);
            shutdown();
            return false;
        }

        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: BEGIN_STREAMING failed: 0x%08X", hr);
            shutdown();
            return false;
        }

        MFT_OUTPUT_STREAM_INFO stream_info{};
        hr = mft_->GetOutputStreamInfo(0, &stream_info);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: GetOutputStreamInfo failed: 0x%08X", hr);
            shutdown();
            return false;
        }
        output_provides_samples_ =
            (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        output_sample_size_ = stream_info.cbSize != 0
            ? stream_info.cbSize : 64u * 1024u;

        passthrough_ = false;
        timeline_initialized_ = false;
        next_output_pts_ = 0;
        output_duration_remainder_ = 0;
        SR_LOG_INFO(L"AudioResampler: %u Hz -> %u Hz, %u ch, %u-bit",
                    input_rate, output_rate, channels, bits);
        return true;
    }

    // Appends converted PCM to out_pcm. Silent input is fed through the same
    // resampler as ordinary PCM, so its duration and filter state stay aligned.
    // input_pts_100ns is on the recording session timeline; output_pts_100ns
    // receives the timestamp of the first output frame appended by this call.
    bool process(const uint8_t* input_data, uint32_t input_bytes,
                 std::vector<uint8_t>& out_pcm, bool input_silence = false,
                 int64_t input_pts_100ns = 0, int64_t* output_pts_100ns = nullptr) {
        if (output_pts_100ns) *output_pts_100ns = input_pts_100ns;
        if (input_bytes == 0 || input_block_align_ == 0 ||
            input_bytes % input_block_align_ != 0 || (!input_data && !input_silence)) {
            SR_LOG_ERROR(L"AudioResampler: invalid input buffer (%u bytes)", input_bytes);
            return false;
        }

        if (passthrough_) {
            try {
                if (input_silence) {
                    out_pcm.insert(out_pcm.end(), input_bytes, uint8_t{0});
                } else {
                    out_pcm.insert(out_pcm.end(), input_data, input_data + input_bytes);
                }
            } catch (...) {
                SR_LOG_ERROR(L"AudioResampler: could not append passthrough PCM");
                return false;
            }
            return true;
        }
        if (!mft_) return false;

        const uint8_t* submitted_data = input_data;
        if (input_silence) {
            try {
                silence_input_.resize(input_bytes);
            } catch (...) {
                SR_LOG_ERROR(L"AudioResampler: could not allocate silent input buffer");
                return false;
            }
            std::fill(silence_input_.begin(), silence_input_.end(), uint8_t{0});
            submitted_data = silence_input_.data();
        }

        ComPtr<IMFSample> input_sample;
        if (!create_input_sample(submitted_data, input_bytes, input_pts_100ns, input_sample)) {
            return false;
        }

        int64_t first_output_pts = input_pts_100ns;
        bool produced_output = false;
        bool drain_failed = false;
        HRESULT hr = audio_resampler_detail::submit_with_drain_retry(
            [&]() { return mft_->ProcessInput(0, input_sample.Get(), 0); },
            [&]() {
                SR_LOG_WARN(L"AudioResampler: input backpressure; draining output before retry");
                const bool drained = drain_output(
                    out_pcm, input_pts_100ns, first_output_pts, produced_output);
                drain_failed = !drained;
                return drained;
            }, 3);
        if (drain_failed) {
            if (output_pts_100ns && produced_output) *output_pts_100ns = first_output_pts;
            return false;
        }
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: ProcessInput failed after bounded retry: 0x%08X", hr);
            if (output_pts_100ns && produced_output) *output_pts_100ns = first_output_pts;
            return false;
        }

        if (!timeline_initialized_) {
            next_output_pts_ = input_pts_100ns;
            timeline_initialized_ = true;
        }

        if (!drain_output(out_pcm, input_pts_100ns, first_output_pts, produced_output)) {
            if (output_pts_100ns && produced_output) *output_pts_100ns = first_output_pts;
            return false;
        }
        if (output_pts_100ns) {
            *output_pts_100ns = produced_output ? first_output_pts
                : (timeline_initialized_ ? next_output_pts_ : input_pts_100ns);
        }
        return true;
    }

    bool flush(std::vector<uint8_t>& out_pcm, int64_t* output_pts_100ns = nullptr) {
        if (output_pts_100ns) *output_pts_100ns = timeline_initialized_ ? next_output_pts_ : 0;
        if (passthrough_ || !mft_) return true;

        HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: END_OF_STREAM failed: 0x%08X", hr);
            return false;
        }
        hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: COMMAND_DRAIN failed: 0x%08X", hr);
            return false;
        }

        int64_t first_output_pts = timeline_initialized_ ? next_output_pts_ : 0;
        bool produced_output = false;
        const bool ok = drain_output(out_pcm, first_output_pts,
                                     first_output_pts, produced_output);
        if (output_pts_100ns && produced_output) *output_pts_100ns = first_output_pts;
        return ok;
    }

    void shutdown() {
        if (mft_) {
            const HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            if (FAILED(hr)) {
                SR_LOG_WARN(L"AudioResampler: END_STREAMING failed during shutdown: 0x%08X", hr);
            }
            mft_.Reset();
        }
        passthrough_ = false;
        output_provides_samples_ = false;
        timeline_initialized_ = false;
        output_duration_remainder_ = 0;
        silence_input_.clear();
    }

    bool is_passthrough() const { return passthrough_; }
    uint32_t input_rate() const { return in_rate_; }
    uint32_t output_rate() const { return out_rate_; }
    uint16_t channels() const { return channels_; }
    uint32_t bits() const { return bits_; }

private:
    bool create_input_sample(const uint8_t* data, uint32_t bytes, int64_t pts,
                             ComPtr<IMFSample>& sample) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: MFCreateMemoryBuffer(input) failed: 0x%08X", hr);
            return false;
        }

        BYTE* destination = nullptr;
        hr = buffer->Lock(&destination, nullptr, nullptr);
        if (FAILED(hr) || !destination) {
            SR_LOG_ERROR(L"AudioResampler: input buffer Lock failed: 0x%08X", hr);
            return false;
        }
        std::memcpy(destination, data, bytes);
        hr = buffer->Unlock();
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: input buffer Unlock failed: 0x%08X", hr);
            return false;
        }
        hr = buffer->SetCurrentLength(bytes);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetCurrentLength(input) failed: 0x%08X", hr);
            return false;
        }

        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: MFCreateSample(input) failed: 0x%08X", hr);
            return false;
        }
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: AddBuffer(input) failed: 0x%08X", hr);
            sample.Reset();
            return false;
        }
        const int64_t input_frames = bytes / input_block_align_;
        const int64_t duration = input_frames * 10'000'000LL / in_rate_;
        hr = sample->SetSampleTime(pts);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetSampleTime(input) failed: 0x%08X", hr);
            sample.Reset();
            return false;
        }
        hr = sample->SetSampleDuration(duration);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetSampleDuration(input) failed: 0x%08X", hr);
            sample.Reset();
            return false;
        }
        return true;
    }

    bool drain_output(std::vector<uint8_t>& out_pcm, int64_t fallback_pts,
                      int64_t& first_output_pts, bool& produced_output) {
        while (true) {
            ComPtr<IMFSample> caller_sample;
            MFT_OUTPUT_DATA_BUFFER output{};
            if (!output_provides_samples_) {
                ComPtr<IMFMediaBuffer> buffer;
                HRESULT hr = MFCreateSample(&caller_sample);
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"AudioResampler: MFCreateSample(output) failed: 0x%08X", hr);
                    return false;
                }
                hr = MFCreateMemoryBuffer(output_sample_size_, &buffer);
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"AudioResampler: MFCreateMemoryBuffer(output) failed: 0x%08X", hr);
                    return false;
                }
                hr = caller_sample->AddBuffer(buffer.Get());
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"AudioResampler: AddBuffer(output) failed: 0x%08X", hr);
                    return false;
                }
                output.pSample = caller_sample.Get();
            }

            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (output.pEvents) output.pEvents->Release();
                return true;
            }
            if (FAILED(hr)) {
                SR_LOG_ERROR(L"AudioResampler: ProcessOutput failed: 0x%08X", hr);
                if (output.pSample && output.pSample != caller_sample.Get()) output.pSample->Release();
                if (output.pEvents) output.pEvents->Release();
                return false;
            }

            ComPtr<IMFSample> sample;
            if (output.pSample && output.pSample != caller_sample.Get()) {
                sample.Attach(output.pSample);
            } else if (caller_sample) {
                sample = caller_sample;
            }
            if (output.pEvents) output.pEvents->Release();
            if (!sample) {
                SR_LOG_ERROR(L"AudioResampler: ProcessOutput succeeded without a sample");
                return false;
            }

            const int64_t packet_pts = timeline_initialized_ ? next_output_pts_ : fallback_pts;

            ComPtr<IMFMediaBuffer> contiguous;
            hr = sample->ConvertToContiguousBuffer(&contiguous);
            if (FAILED(hr) || !contiguous) {
                SR_LOG_ERROR(L"AudioResampler: ConvertToContiguousBuffer failed: 0x%08X", hr);
                return false;
            }
            BYTE* bytes = nullptr;
            DWORD max_length = 0;
            DWORD current_length = 0;
            hr = contiguous->Lock(&bytes, &max_length, &current_length);
            if (FAILED(hr) || (!bytes && current_length != 0)) {
                SR_LOG_ERROR(L"AudioResampler: output buffer Lock failed: 0x%08X", hr);
                return false;
            }
            bool append_ok = true;
            if (current_length != 0) {
                if (current_length % output_block_align_ != 0) {
                    SR_LOG_ERROR(L"AudioResampler: output length %u is not frame aligned", current_length);
                    append_ok = false;
                } else {
                    try {
                        out_pcm.insert(out_pcm.end(), bytes, bytes + current_length);
                    } catch (...) {
                        SR_LOG_ERROR(L"AudioResampler: could not append output PCM");
                        append_ok = false;
                    }
                }
            }
            const HRESULT unlock_hr = contiguous->Unlock();
            if (FAILED(unlock_hr)) {
                SR_LOG_ERROR(L"AudioResampler: output buffer Unlock failed: 0x%08X", unlock_hr);
                append_ok = false;
            }
            if (!append_ok) return false;
            if (current_length == 0) {
                SR_LOG_WARN(L"AudioResampler: MFT returned an empty output sample; ending this drain pass");
                return true;
            }

            const uint64_t frames = current_length / output_block_align_;
            if (!produced_output) {
                first_output_pts = packet_pts;
                produced_output = true;
            }
            const uint64_t duration_numerator = frames * 10'000'000ULL + output_duration_remainder_;
            const int64_t sample_duration = static_cast<int64_t>(duration_numerator / out_rate_);
            output_duration_remainder_ = duration_numerator % out_rate_;
            next_output_pts_ = packet_pts + sample_duration;
            timeline_initialized_ = true;
        }
    }

    static HRESULT set_pcm_type(IMFMediaType* type, uint32_t rate,
                                uint16_t channels, uint32_t bits) {
        if (!type) return E_POINTER;
        HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (FAILED(hr)) return hr;
        hr = type->SetGUID(MF_MT_SUBTYPE, bits == 32 ? MFAudioFormat_Float : MFAudioFormat_PCM);
        if (FAILED(hr)) return hr;
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        if (FAILED(hr)) return hr;
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (FAILED(hr)) return hr;
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
        if (FAILED(hr)) return hr;
        hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * (bits / 8));
        if (FAILED(hr)) return hr;
        hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                             rate * channels * (bits / 8));
        if (FAILED(hr)) return hr;
        return type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }

    ComPtr<IMFTransform> mft_;
    uint32_t in_rate_ = 44'100;
    uint32_t out_rate_ = 48'000;
    uint16_t channels_ = 2;
    uint32_t bits_ = 16;
    uint32_t input_block_align_ = 4;
    uint32_t output_block_align_ = 4;
    uint32_t output_sample_size_ = 64u * 1024u;
    bool passthrough_ = false;
    bool output_provides_samples_ = false;
    bool timeline_initialized_ = false;
    int64_t next_output_pts_ = 0;
    uint64_t output_duration_remainder_ = 0;
    std::vector<uint8_t> silence_input_;
};

} // namespace sr
