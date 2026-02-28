// video_encoder.cpp — Media Foundation H.264 encoder with 3-step fallback
// T013: CBR, 2s GOP, low-latency, no B-frames, Baseline/Main profile
// Chain: HW MFT -> SW MFT (1080p) -> SW MFT (720p30)

#include "encoder/video_encoder.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>    // MF_E_NOTACCEPTING, MF_E_TRANSFORM_NEED_MORE_INPUT
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <d3d11_1.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sr {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static HRESULT SetUINT32Attr(IMFAttributes* a, REFGUID k, UINT32 v) {
    return a->SetUINT32(k, v);
}
static HRESULT SetUINT64Attr(IMFAttributes* a, REFGUID k, UINT64 v) {
    return a->SetUINT64(k, v);
}

// Configure output H.264 media type on the MFT
static HRESULT ConfigureOutputType(IMFTransform* mft,
                                   uint32_t w, uint32_t h,
                                   uint32_t fps_num, uint32_t fps_den,
                                   uint32_t bitrate)
{
    ComPtr<IMFMediaType> out_type;
    HRESULT hr = MFCreateMediaType(&out_type);
    if (FAILED(hr)) return hr;

    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out_type->SetUINT32(MF_MT_AVG_BITRATE,            bitrate);
    out_type->SetUINT32(MF_MT_INTERLACE_MODE,         MFVideoInterlace_Progressive);
    out_type->SetUINT32(MF_MT_MPEG2_PROFILE,          eAVEncH264VProfile_Main);
    out_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,    MFNominalRange_16_235);

    return mft->SetOutputType(0, out_type.Get(), 0);
}

// Configure input NV12 media type on the MFT
static HRESULT ConfigureInputType(IMFTransform* mft,
                                   uint32_t w, uint32_t h,
                                   uint32_t fps_num, uint32_t fps_den)
{
    ComPtr<IMFMediaType> in_type;
    HRESULT hr = MFCreateMediaType(&in_type);
    if (FAILED(hr)) return hr;

    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    return mft->SetInputType(0, in_type.Get(), 0);
}

// Apply encoder codec attributes (CBR, low-latency, no B-frames, 2s GOP)
static void ApplyEncoderAttributes(IMFTransform* mft, uint32_t fps,
                                   uint32_t bitrate, bool is_hw)
{
    ComPtr<ICodecAPI> codec_api;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&codec_api)))) return;

    // Rate control mode: CBR
    VARIANT v{};
    v.vt   = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_CBR;
    codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);

    // Bitrate
    v.vt   = VT_UI4;
    v.ulVal = bitrate;
    codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

    // Low latency
    v.vt      = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);

    // No B-frames
    v.vt   = VT_UI4;
    v.ulVal = 0;
    codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

    // GOP size = 2 * fps
    v.vt   = VT_UI4;
    v.ulVal = fps * 2;
    codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

    (void)is_hw;
}

// ---------------------------------------------------------------------------
// VideoEncoder::try_init_hw
// ---------------------------------------------------------------------------
bool VideoEncoder::try_init_hw(const EncoderProfile& profile,
                                IMFDXGIDeviceManager* dxgi_mgr)
{
    if (!dxgi_mgr) return false;

    MFT_REGISTER_TYPE_INFO out_info{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &out_info,
        &activates, &count);

    if (FAILED(hr) || count == 0) {
        SR_LOG_INFO(L"No hardware H.264 encoder found");
        if (activates) { for (UINT32 i = 0; i < count; ++i) activates[i]->Release(); CoTaskMemFree(activates); }
        return false;
    }

    // Try each HW encoder
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFTransform> mft;
        if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&mft)))) continue;

        // Attach DXGI device manager
        ComPtr<IMFAttributes> attrs;
        mft->GetAttributes(&attrs);
        if (attrs) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }

        // Set DXGI device manager
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(dxgi_mgr));
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT %u: SetD3DManager failed (0x%08X), skipping", i, hr);
            activates[i]->ShutdownObject();
            continue;
        }

        WCHAR name[256]{};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);

        // Configure output then input types
        hr = ConfigureOutputType(mft.Get(), profile.width, profile.height,
                                  profile.fps, 1, profile.bitrate_bps);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': SetOutputType failed (0x%08X)", name, hr);
            activates[i]->ShutdownObject();
            continue;
        }

        hr = ConfigureInputType(mft.Get(), profile.width, profile.height, profile.fps, 1);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': SetInputType failed (0x%08X)", name, hr);
            activates[i]->ShutdownObject();
            continue;
        }

        ApplyEncoderAttributes(mft.Get(), profile.fps, profile.bitrate_bps, true);

        hr = mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

        mft_  = mft;
        mode_ = EncoderMode::HardwareMFT;
        out_width_  = profile.width;
        out_height_ = profile.height;
        out_fps_    = profile.fps;
        hw_path_    = true;

        SR_LOG_INFO(L"HW H.264 encoder active: %s (%ux%u @ %u fps, %u bps)",
            name, profile.width, profile.height, profile.fps, profile.bitrate_bps);

        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return true;
    }

    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return false;
}

// ---------------------------------------------------------------------------
// VideoEncoder::try_init_sw
// ---------------------------------------------------------------------------
bool VideoEncoder::try_init_sw(const EncoderProfile& profile,
                                uint32_t width, uint32_t height, uint32_t fps)
{
    MFT_REGISTER_TYPE_INFO out_info{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &out_info,
        &activates, &count);

    if (FAILED(hr) || count == 0) {
        SR_LOG_ERROR(L"No software H.264 encoder found");
        if (activates) { for (UINT32 i = 0; i < count; ++i) activates[i]->Release(); CoTaskMemFree(activates); }
        return false;
    }

    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFTransform> mft;
        if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&mft)))) continue;

        WCHAR name[256]{};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr);

        hr = ConfigureOutputType(mft.Get(), width, height, fps, 1,
                                  profile.bitrate_bps);
        if (FAILED(hr)) { activates[i]->ShutdownObject(); continue; }

        hr = ConfigureInputType(mft.Get(), width, height, fps, 1);
        if (FAILED(hr)) { activates[i]->ShutdownObject(); continue; }

        ApplyEncoderAttributes(mft.Get(), fps, profile.bitrate_bps, false);

        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

        mft_       = mft;
        out_width_  = width;
        out_height_ = height;
        out_fps_    = fps;
        hw_path_    = false;

        if (width == 1280) {
            mode_ = EncoderMode::SoftwareMFT720p;
            SR_LOG_WARN(L"SW H.264 (720p30 degraded fallback): %s", name);
        } else {
            mode_ = EncoderMode::SoftwareMFT;
            SR_LOG_INFO(L"SW H.264 encoder: %s (%ux%u @ %u fps)", name, width, height, fps);
        }

        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return true;
    }

    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return false;
}

// ---------------------------------------------------------------------------
// VideoEncoder::initialize — 3-step fallback chain
// ---------------------------------------------------------------------------
bool VideoEncoder::initialize(const EncoderProfile& profile,
                               IMFDXGIDeviceManager* dxgi_mgr,
                               ID3D11Device* d3d_device,
                               ID3D11DeviceContext* d3d_context)
{
    d3d_device_  = d3d_device;
    d3d_context_ = d3d_context;
    if (dxgi_mgr) dxgi_mgr_ = dxgi_mgr;

    // Attempt 1: Hardware MFT
    if (try_init_hw(profile, dxgi_mgr)) {
        initialized_ = true;
        return true;
    }
    SR_LOG_WARN(L"HW encoder failed — trying SW MFT at original resolution");

    // Attempt 2: Software MFT at original resolution
    if (try_init_sw(profile, profile.width, profile.height, profile.fps)) {
        initialized_ = true;
        return true;
    }
    SR_LOG_WARN(L"SW encoder failed — trying 720p30 degraded fallback");

    // Attempt 3: Software MFT at 720p30
    if (try_init_sw(profile, 1280, 720, 30)) {
        initialized_ = true;
        return true;
    }

    SR_LOG_ERROR(L"All encoder attempts failed");
    return false;
}

// ---------------------------------------------------------------------------
// VideoEncoder::encode_frame
// ---------------------------------------------------------------------------
bool VideoEncoder::encode_frame(ID3D11Texture2D* nv12_texture, int64_t pts,
                                 ComPtr<IMFSample>& out_sample)
{
    if (!initialized_ || !mft_) return false;

    // Create an IMFSample wrapping the NV12 texture
    ComPtr<IMFSample>      sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    if (hw_path_) {
        // HW path: wrap D3D11 texture in MF DXGI buffer
        hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D),
            nv12_texture,
            0,      // subresource index
            FALSE,  // bottomUpWhenLinear
            &buffer);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"MFCreateDXGISurfaceBuffer failed: 0x%08X", hr);
            return false;
        }
    } else {
        // SW path: read back texture to system memory
        D3D11_TEXTURE2D_DESC td{};
        nv12_texture->GetDesc(&td);
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags      = 0;

        ComPtr<ID3D11Texture2D> staging;
        hr = d3d_device_->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) return false;

        d3d_context_->CopyResource(staging.Get(), nv12_texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        // NV12: Y plane + interleaved UV plane
        uint32_t y_size  = mapped.RowPitch * td.Height;
        uint32_t uv_size = mapped.RowPitch * (td.Height / 2);
        DWORD total = y_size + uv_size;

        hr = MFCreateMemoryBuffer(total, &buffer);
        BYTE* buf_data = nullptr;
        buffer->Lock(&buf_data, nullptr, nullptr);
        std::memcpy(buf_data, mapped.pData, y_size);
        std::memcpy(buf_data + y_size,
                    static_cast<uint8_t*>(mapped.pData) + y_size,
                    uv_size);
        buffer->Unlock();
        buffer->SetCurrentLength(total);

        d3d_context_->Unmap(staging.Get(), 0);
    }

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(pts);

    // Compute duration (100ns)
    int64_t duration = 10'000'000LL / static_cast<int64_t>(out_fps_);
    sample->SetSampleDuration(duration);

    // Force IDR keyframe if requested (e.g. after resume from pause)
    if (force_keyframe_next_.exchange(false, std::memory_order_acq_rel)) {
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(mft_->QueryInterface(IID_PPV_ARGS(&codec_api)))) {
            VARIANT v{};
            v.vt   = VT_UI4;
            v.ulVal = 1;  // 1 = force next frame as IDR
            codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
            SR_LOG_INFO(L"VideoEncoder: forced IDR keyframe on resume");
        }
    }

    // Feed to MFT
    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        SR_LOG_ERROR(L"ProcessInput failed: 0x%08X", hr);
        return false;
    }

    // Get output
    MFT_OUTPUT_DATA_BUFFER out_buf{};
    DWORD status = 0;
    hr = mft_->ProcessOutput(0, 1, &out_buf, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false; // normal
    if (FAILED(hr)) return false;

    if (out_buf.pSample) {
        out_sample.Attach(out_buf.pSample);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// VideoEncoder::flush
// ---------------------------------------------------------------------------
bool VideoEncoder::flush(std::vector<ComPtr<IMFSample>>& out_samples) {
    if (!initialized_ || !mft_) return false;

    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

    while (true) {
        MFT_OUTPUT_DATA_BUFFER out_buf{};
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) break;
        if (out_buf.pSample) {
            ComPtr<IMFSample> s;
            s.Attach(out_buf.pSample);
            out_samples.push_back(std::move(s));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// VideoEncoder::shutdown
// ---------------------------------------------------------------------------
void VideoEncoder::shutdown() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_.Reset();
    }
    initialized_ = false;
}

} // namespace sr
