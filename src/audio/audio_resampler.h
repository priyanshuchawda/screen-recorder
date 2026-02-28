#pragma once
// audio_resampler.h — MF Resampler MFT wrapper for sample-rate conversion
// T032: Converts device native rate (e.g., 44.1 kHz) to 48 kHz AAC pipeline rate.
// Uses CLSID_CResamplerMediaObject (wmcodecdsp) — high-quality SOXR-like resampler.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>       // CLSID_CResamplerMediaObject
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include "utils/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

namespace sr {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// AudioResampler
// ---------------------------------------------------------------------------
// Wraps the Windows Media Foundation Resampler MFT to perform PCM sample-rate
// conversion from any native device rate to the 48 kHz required by the AAC
// encoder pipeline.
//
// Usage:
//   AudioResampler rs;
//   rs.initialize(44100, 2, 16 /*bits*/, 48000);
//   while (capturing) {
//       std::vector<uint8_t> resampled;
//       rs.process(pcm_data, pcm_bytes, resampled);
//       // push resampled data to AudioQueue
//   }
//   rs.flush(resampled);  // drain tail samples
// ---------------------------------------------------------------------------
class AudioResampler {
public:
    AudioResampler()  = default;
    ~AudioResampler() { shutdown(); }

    // Initialize the resampler.
    //   in_rate      — input sample rate (e.g., 44100)
    //   channels     — channel count (1 or 2)
    //   bits         — bits per sample (16 or 32)
    //   out_rate     — output sample rate (default 48000)
    // Returns false if the device already runs at out_rate (no-op pass-through
    // is indicated by is_passthrough() == true).
    bool initialize(uint32_t in_rate,
                    uint16_t channels,
                    uint32_t bits,
                    uint32_t out_rate = 48000)
    {
        in_rate_  = in_rate;
        out_rate_ = out_rate;
        channels_ = channels;
        bits_     = bits;

        // If rates match, operate as passthrough — no MFT needed.
        if (in_rate == out_rate) {
            passthrough_ = true;
            SR_LOG_INFO(L"AudioResampler: same rate (%u Hz) — passthrough mode", in_rate);
            return true;
        }

        passthrough_ = false;

        // --- Create Resampler MFT (CLSID_CResamplerMediaObject) ---
        HRESULT hr = CoCreateInstance(
            CLSID_CResamplerMediaObject,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&mft_)
        );
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: CoCreateInstance(CResamplerMediaObject) failed: 0x%08X", hr);
            return false;
        }

        // --- Configure input media type ---
        ComPtr<IMFMediaType> in_type;
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) return false;
        if (!set_pcm_type(in_type.Get(), in_rate, channels, bits)) return false;
        hr = mft_->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetInputType failed: 0x%08X", hr);
            return false;
        }

        // --- Configure output media type ---
        ComPtr<IMFMediaType> out_type;
        hr = MFCreateMediaType(&out_type);
        if (FAILED(hr)) return false;
        if (!set_pcm_type(out_type.Get(), out_rate, channels, bits)) return false;
        hr = mft_->SetOutputType(0, out_type.Get(), 0);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"AudioResampler: SetOutputType failed: 0x%08X", hr);
            return false;
        }

        // --- Begin streaming ---
        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"AudioResampler: NotifyBeginStreaming failed (non-fatal): 0x%08X", hr);
        }

        // Cache output stream info for buffer allocation
        MFT_OUTPUT_STREAM_INFO out_stream_info{};
        mft_->GetOutputStreamInfo(0, &out_stream_info);
        out_sample_size_ = out_stream_info.cbSize > 0
                           ? out_stream_info.cbSize
                           : 4096; // safe default

        SR_LOG_INFO(L"AudioResampler: %u Hz -> %u Hz, %u ch, %u-bit",
                    in_rate, out_rate, channels, bits);
        return true;
    }

    // Process one block of PCM audio.
    // in_data / in_bytes — raw PCM at in_rate
    // out_pcm            — resampled PCM appended at out_rate
    bool process(const uint8_t* in_data, uint32_t in_bytes,
                 std::vector<uint8_t>& out_pcm)
    {
        if (passthrough_) {
            // No-op: caller uses audio at native rate unchanged
            out_pcm.insert(out_pcm.end(), in_data, in_data + in_bytes);
            return true;
        }
        if (!mft_) return false;

        // --- Wrap input in IMFMediaBuffer / IMFSample ---
        ComPtr<IMFMediaBuffer> in_buf;
        HRESULT hr = MFCreateMemoryBuffer(in_bytes, &in_buf);
        if (FAILED(hr)) return false;

        BYTE* ptr = nullptr;
        in_buf->Lock(&ptr, nullptr, nullptr);
        memcpy(ptr, in_data, in_bytes);
        in_buf->Unlock();
        in_buf->SetCurrentLength(in_bytes);

        ComPtr<IMFSample> in_sample;
        hr = MFCreateSample(&in_sample);
        if (FAILED(hr)) return false;
        in_sample->AddBuffer(in_buf.Get());

        hr = mft_->ProcessInput(0, in_sample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            SR_LOG_ERROR(L"AudioResampler: ProcessInput failed: 0x%08X", hr);
            return false;
        }

        // --- Pull all available output ---
        return drain_output(out_pcm);
    }

    // Drain remaining samples (call at end of recording or on device change)
    bool flush(std::vector<uint8_t>& out_pcm) {
        if (passthrough_ || !mft_) return true;

        HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        (void)hr;

        return drain_output(out_pcm);
    }

    void shutdown() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft_.Reset();
        }
    }

    // Accessors
    bool     is_passthrough() const { return passthrough_; }
    uint32_t input_rate()     const { return in_rate_; }
    uint32_t output_rate()    const { return out_rate_; }
    uint16_t channels()       const { return channels_; }
    uint32_t bits()           const { return bits_; }

private:
    // Set PCM uncompressed media type attributes
    static bool set_pcm_type(IMFMediaType* type,
                              uint32_t rate, uint16_t channels, uint32_t bits)
    {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        // Use float for 32-bit, int for 16-bit
        type->SetGUID(MF_MT_SUBTYPE,
                      (bits == 32) ? MFAudioFormat_Float : MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       channels);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    bits);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,    (channels * bits) / 8);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                        rate * channels * bits / 8);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        return true;
    }

    // Pull output samples from MFT until MF_E_TRANSFORM_NEED_MORE_INPUT
    bool drain_output(std::vector<uint8_t>& out_pcm) {
        while (true) {
            // Allocate output sample
            ComPtr<IMFMediaBuffer> out_buf;
            MFCreateMemoryBuffer(out_sample_size_, &out_buf);

            ComPtr<IMFSample> out_sample;
            MFCreateSample(&out_sample);
            out_sample->AddBuffer(out_buf.Get());

            MFT_OUTPUT_DATA_BUFFER obuf{};
            obuf.pSample = out_sample.Get();

            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &obuf, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break; // nothing more
            if (FAILED(hr)) {
                SR_LOG_WARN(L"AudioResampler: ProcessOutput: 0x%08X", hr);
                break;
            }

            // Append output PCM bytes
            ComPtr<IMFMediaBuffer> contiguous;
            out_sample->ConvertToContiguousBuffer(&contiguous);
            if (contiguous) {
                BYTE*  data = nullptr;
                DWORD  len  = 0;
                contiguous->Lock(&data, nullptr, &len);
                if (data && len > 0) {
                    out_pcm.insert(out_pcm.end(), data, data + len);
                }
                contiguous->Unlock();
            }
        }
        return true;
    }

    ComPtr<IMFTransform> mft_;
    uint32_t in_rate_          = 44100;
    uint32_t out_rate_         = 48000;
    uint16_t channels_         = 2;
    uint32_t bits_             = 16;
    uint32_t out_sample_size_  = 4096;
    bool     passthrough_      = false;
};

} // namespace sr
