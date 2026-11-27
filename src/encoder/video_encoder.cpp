// video_encoder.cpp — Media Foundation H.264 encoder with 3-step fallback
// T013: CBR, ~1s GOP, low-latency, no B-frames, Baseline/Main profile
// Chain: HW MFT -> SW MFT (1080p) -> SW MFT (720p30)

#include "encoder/video_encoder.h"
#include "encoder/drain_result.h"
#include "encoder/tracked_sample_allocator.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>    // MF_E_NOTACCEPTING, MF_E_TRANSFORM_NEED_MORE_INPUT
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <cstdlib>
#include <atomic>

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

static HRESULT CreateMftOutputSample(uint32_t sample_size, IMFSample** sample_out) {
    if (!sample_out) return E_POINTER;
    *sample_out = nullptr;

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    hr = MFCreateMemoryBuffer(sample_size, &buffer);
    if (FAILED(hr)) return hr;

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;

    *sample_out = sample.Detach();
    return S_OK;
}

static void LogProcessOutputFailureRateLimited(HRESULT hr) {
    static std::atomic<uint32_t> fail_count{ 0 };
    static std::atomic<long> last_hr{ S_OK };

    const uint32_t count = fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const long prev_hr = last_hr.exchange(static_cast<long>(hr), std::memory_order_relaxed);

    if (count == 1 || prev_hr != static_cast<long>(hr)) {
        SR_LOG_ERROR(L"ProcessOutput failed: 0x%08X (count=%u)", hr, count);
        return;
    }

    if ((count % 120) == 0) {
        SR_LOG_WARN(L"ProcessOutput failed repeatedly (0x%08X), total=%u", hr, count);
    }
}

bool VideoEncoder::pump_hw_events(uint32_t wait_ms) {
    if (!hw_async_mft_ || !hw_events_) {
        return true;
    }

    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    bool saw_event = false;

    while (true) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = hw_events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (wait_ms == 0 || saw_event || GetTickCount64() >= deadline) {
                return true;
            }
            Sleep(1);
            continue;
        }
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT GetEvent failed: 0x%08X", hr);
            return false;
        }

        saw_event = true;

        HRESULT status = S_OK;
        const HRESULT status_hr = event->GetStatus(&status);
        if (FAILED(status_hr) || FAILED(status)) {
            SR_LOG_ERROR(L"HW MFT event status failed: get=0x%08X status=0x%08X", status_hr, status);
            return false;
        }

        MediaEventType type = MEUnknown;
        const HRESULT type_hr = event->GetType(&type);
        if (FAILED(type_hr)) {
            SR_LOG_ERROR(L"HW MFT event GetType failed: 0x%08X", type_hr);
            return false;
        }

        switch (type) {
            case METransformNeedInput:
                ++hw_need_input_events_;
                break;
            case METransformHaveOutput:
                ++hw_have_output_events_;
                break;
            case METransformDrainComplete:
                hw_drain_complete_ = true;
                break;
            default:
                break;
        }
    }
}

DeferredSubmitSummary VideoEncoder::submit_deferred_inputs() {
    auto summary = submit_ready_inputs(
        deferred_inputs_, hw_need_input_events_, [this](DeferredInput& input) {
            const HRESULT hr = mft_->ProcessInput(0, input.sample.Get(), 0);
            if (SUCCEEDED(hr)) {
                input_samples_accepted_.fetch_add(1, std::memory_order_relaxed);
                pending_video_pts_.push_back(input.pts);
                const uint64_t pending = pending_input_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                uint64_t high = pending_input_high_watermark_.load(std::memory_order_relaxed);
                while (pending > high && !pending_input_high_watermark_.compare_exchange_weak(
                           high, pending, std::memory_order_relaxed)) {}
                if (input.was_deferred) {
                    input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                }
                return DeferredSubmitResult::Accepted;
            }
            if (hr == MF_E_NOTACCEPTING) {
                input_samples_not_accepted_.fetch_add(1, std::memory_order_relaxed);
                SR_LOG_WARN(L"Async encoder returned MF_E_NOTACCEPTING after a need-input event; retaining sample for retry");
                return DeferredSubmitResult::NotAccepted;
            }

            input_errors_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"ProcessInput for deferred frame failed: 0x%08X", hr);
            return DeferredSubmitResult::Failed;
        });

    if (summary.failed != 0) {
        // A fatal ProcessInput error cannot be repaired by waiting for another
        // event. Drop only that sample, account it, and keep later inputs queued.
        deferred_inputs_.pop_front();
        pending_inputs_abandoned_.fetch_add(summary.failed, std::memory_order_relaxed);
    }
    update_deferred_input_metrics();
    return summary;
}

bool VideoEncoder::remove_deferred_input(IMFSample* sample) {
    if (!sample) return false;
    const bool removed = deferred_inputs_.erase_first([sample](const DeferredInput& pending) {
        return pending.sample.Get() == sample;
    });
    if (removed) update_deferred_input_metrics();
    return removed;
}

uint64_t VideoEncoder::abandon_deferred_inputs() noexcept {
    const uint64_t count = static_cast<uint64_t>(deferred_inputs_.size());
    deferred_inputs_.clear();
    if (count != 0) {
        pending_inputs_abandoned_.fetch_add(count, std::memory_order_relaxed);
    }
    update_deferred_input_metrics();
    return count;
}

void VideoEncoder::update_deferred_input_metrics() noexcept {
    const uint64_t pending = static_cast<uint64_t>(deferred_inputs_.size());
    deferred_input_count_.store(pending, std::memory_order_relaxed);
    uint64_t high = deferred_input_high_watermark_.load(std::memory_order_relaxed);
    while (pending > high && !deferred_input_high_watermark_.compare_exchange_weak(
               high, pending, std::memory_order_relaxed)) {}
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
    in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    in_type->SetUINT32(MF_MT_SAMPLE_SIZE, (w * h * 3) / 2);

    return mft->SetInputType(0, in_type.Get(), 0);
}

// Apply encoder codec attributes (CBR, low-latency, no B-frames, ~1s GOP)
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

    // GOP size = 1 * fps (about 1 second)
    v.vt   = VT_UI4;
    v.ulVal = fps;
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
    hw_events_.Reset();
    hw_async_mft_ = false;
    hw_need_input_events_ = 0;
    hw_have_output_events_ = 0;
    hw_drain_complete_ = false;

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
            UINT32 is_async = FALSE;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
            if (is_async) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
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
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        mft_  = mft;
        mode_ = EncoderMode::HardwareMFT;
        out_width_  = profile.width;
        out_height_ = profile.height;
        out_fps_    = profile.fps;
        hw_path_    = true;
        hw_async_mft_ = SUCCEEDED(mft.As(&hw_events_)) && hw_events_;

        MFT_OUTPUT_STREAM_INFO output_stream_info{};
        hr = mft_->GetOutputStreamInfo(0, &output_stream_info);
        if (SUCCEEDED(hr)) {
            mft_provides_output_samples_ = (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            mft_output_sample_size_ = output_stream_info.cbSize > 0 ? output_stream_info.cbSize : (1 << 20);
        } else {
            mft_provides_output_samples_ = true;
            mft_output_sample_size_ = 1 << 20;
        }

        if (hw_async_mft_) {
            pump_hw_events(50);
        }
        last_output_tick_ms_ = GetTickCount64();

        SR_LOG_INFO(L"HW H.264 encoder active: %s (%ux%u @ %u fps, %u bps, async=%s)",
            name, profile.width, profile.height, profile.fps, profile.bitrate_bps,
            hw_async_mft_ ? L"yes" : L"no");

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
    hw_events_.Reset();
    hw_async_mft_ = false;
    hw_need_input_events_ = 0;
    hw_have_output_events_ = 0;
    hw_drain_complete_ = false;

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
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        mft_       = mft;
        out_width_  = width;
        out_height_ = height;
        out_fps_    = fps;
        hw_path_    = false;
        last_output_tick_ms_ = GetTickCount64();

        MFT_OUTPUT_STREAM_INFO output_stream_info{};
        hr = mft_->GetOutputStreamInfo(0, &output_stream_info);
        if (SUCCEEDED(hr)) {
            mft_provides_output_samples_ = (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            mft_output_sample_size_ = output_stream_info.cbSize > 0 ? output_stream_info.cbSize : (1 << 20);
        } else {
            mft_provides_output_samples_ = true;
            mft_output_sample_size_ = 1 << 20;
        }

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

// VideoEncoder::initialize — 3-step deterministic fallback chain (T035)
// Each attempt is explicitly logged so diagnostics are always clear.
// Chain: [1] HW MFT → [2] SW MFT same-res → [3] SW MFT 720p30
// ---------------------------------------------------------------------------
bool VideoEncoder::initialize(const EncoderProfile& profile,
                               IMFDXGIDeviceManager* dxgi_mgr,
                               ID3D11Device* d3d_device,
                               ID3D11DeviceContext* d3d_context)
{
    shutdown();
    input_samples_offered_.store(0, std::memory_order_relaxed);
    input_samples_accepted_.store(0, std::memory_order_relaxed);
    input_samples_not_accepted_.store(0, std::memory_order_relaxed);
    input_samples_deferred_without_event_.store(0, std::memory_order_relaxed);
    input_errors_.store(0, std::memory_order_relaxed);
    output_samples_produced_.store(0, std::memory_order_relaxed);
    output_waits_for_input_.store(0, std::memory_order_relaxed);
    pending_input_count_.store(0, std::memory_order_relaxed);
    pending_input_high_watermark_.store(0, std::memory_order_relaxed);
    pending_inputs_abandoned_.store(0, std::memory_order_relaxed);
    input_samples_retried_.store(0, std::memory_order_relaxed);
    output_timestamp_errors_.store(0, std::memory_order_relaxed);
    pending_video_pts_.clear();
    deferred_inputs_.reset();
    deferred_input_count_.store(0, std::memory_order_relaxed);
    deferred_input_high_watermark_.store(0, std::memory_order_relaxed);

    d3d_device_  = d3d_device;
    d3d_context_ = d3d_context;
    if (dxgi_mgr) dxgi_mgr_ = dxgi_mgr;

    SR_LOG_INFO(L"[Encoder Fallback Chain] Starting — profile: %ux%u @ %u fps, %u bps",
                profile.width, profile.height, profile.fps, profile.bitrate_bps);

    active_bitrate_bps_ = profile.bitrate_bps;
    hw_output_fail_count_ = 0;
    switched_to_sw_due_to_hw_errors_ = false;

    wchar_t* order_env = nullptr;
    size_t order_env_len = 0;
    _wdupenv_s(&order_env, &order_env_len, L"SR_ENCODER_ORDER");
    bool prefer_hw_first = true;
    if (order_env) {
        if (_wcsicmp(order_env, L"sw-first") == 0 ||
            _wcsicmp(order_env, L"stability-first") == 0)
        {
            prefer_hw_first = false;
        } else if (_wcsicmp(order_env, L"hw-first") == 0) {
            prefer_hw_first = true;
        }
    }
    if (order_env) {
        free(order_env);
    }

    if (prefer_hw_first) {
        SR_LOG_INFO(L"[Encoder Fallback Chain] Mode: HW-first (default)");

        SR_LOG_INFO(L"[Encoder Fallback Chain] Step 1: Hardware MFT...");
        if (try_init_hw(profile, dxgi_mgr)) {
            SR_LOG_INFO(L"[Encoder Fallback Chain] Step 1 SUCCESS — using HW encoder");
            initialized_ = true;
            publish_configuration();
            return true;
        }
        SR_LOG_WARN(L"[Encoder Fallback Chain] Step 1 FAILED — HW encoder unavailable or unstable");

        SR_LOG_INFO(L"[Encoder Fallback Chain] Step 2: SW MFT (%ux%u @ %u fps)...",
                    profile.width, profile.height, profile.fps);
        if (try_init_sw(profile, profile.width, profile.height, profile.fps)) {
            SR_LOG_INFO(L"[Encoder Fallback Chain] Step 2 SUCCESS — using SW encoder (original res)");
            initialized_ = true;
            publish_configuration();
            return true;
        }
        SR_LOG_WARN(L"[Encoder Fallback Chain] Step 2 FAILED — SW encoder at original resolution failed");

        SR_LOG_WARN(L"[Encoder Fallback Chain] Step 3: SW MFT 720p30 (degraded)...");
        if (try_init_sw(profile, 1280, 720, 30)) {
            SR_LOG_WARN(L"[Encoder Fallback Chain] Step 3 SUCCESS — using SW 720p30 degraded fallback");
            initialized_ = true;
            publish_configuration();
            return true;
        }
        SR_LOG_ERROR(L"[Encoder Fallback Chain] Step 3 FAILED — ALL encoder attempts exhausted");
        shutdown();
        return false;
    }

    SR_LOG_INFO(L"[Encoder Fallback Chain] Mode: stability-first (env override)");

    // ─── Attempt 1: Software MFT at original resolution (stability-first) ─────
    SR_LOG_INFO(L"[Encoder Fallback Chain] Step 1: SW MFT (%ux%u @ %u fps)...",
                profile.width, profile.height, profile.fps);
    if (try_init_sw(profile, profile.width, profile.height, profile.fps)) {
        SR_LOG_INFO(L"[Encoder Fallback Chain] Step 1 SUCCESS — using SW encoder (original res)");
        initialized_ = true;
        publish_configuration();
        return true;
    }
    SR_LOG_WARN(L"[Encoder Fallback Chain] Step 1 FAILED — SW encoder at original resolution failed");

    // ─── Attempt 2: Hardware MFT (Intel Quick Sync / AMD VCE / NVIDIA NVENC) ───
    SR_LOG_INFO(L"[Encoder Fallback Chain] Step 2: Hardware MFT...");
    if (try_init_hw(profile, dxgi_mgr)) {
        SR_LOG_INFO(L"[Encoder Fallback Chain] Step 2 SUCCESS — using HW encoder");
        initialized_ = true;
        publish_configuration();
        return true;
    }
    SR_LOG_WARN(L"[Encoder Fallback Chain] Step 2 FAILED — HW encoder unavailable or unstable");

    // ─── Attempt 3: Software MFT at 720p30 (degraded safety fallback) ─────────
    SR_LOG_WARN(L"[Encoder Fallback Chain] Step 3: SW MFT 720p30 (degraded)...");
    if (try_init_sw(profile, 1280, 720, 30)) {
        SR_LOG_WARN(L"[Encoder Fallback Chain] Step 3 SUCCESS — using SW 720p30 degraded fallback");
        initialized_ = true;
        publish_configuration();
        return true;
    }
    SR_LOG_ERROR(L"[Encoder Fallback Chain] Step 3 FAILED — ALL encoder attempts exhausted");
    shutdown();
    return false;
}

// ---------------------------------------------------------------------------
// VideoEncoder::encode_frame
// ---------------------------------------------------------------------------
bool VideoEncoder::encode_frame(ID3D11Texture2D* nv12_texture, int64_t pts,
                                 ComPtr<IMFSample>& out_sample,
                                 std::shared_ptr<TextureSurfaceLease> surface_lease)
{
    if (!initialized_ || !mft_) return false;
    input_samples_offered_.fetch_add(1, std::memory_order_relaxed);

    auto recover_stalled_hardware = [&](IMFSample* retry_sample = nullptr) {
        constexpr ULONGLONG kOutputStallLimitMs = 5'000;
        if (!hw_path_ || switched_to_sw_due_to_hw_errors_ ||
            last_output_tick_ms_ == 0 ||
            GetTickCount64() - last_output_tick_ms_ < kOutputStallLimitMs) {
            return false;
        }
        SR_LOG_ERROR(L"Hardware encoder produced no output for %llu ms; switching to software fallback",
                     GetTickCount64() - last_output_tick_ms_);
        if (retry_sample) remove_deferred_input(retry_sample);
        return switch_to_software_fallback();
    };

    // Create an IMFSample wrapping the NV12 texture
    ComPtr<IMFSample>      sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = hw_path_
        ? create_tracked_surface_sample(surface_lease, &sample)
        : MFCreateSample(&sample);
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
            if (recover_stalled_hardware()) {
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false;
        }

        D3D11_TEXTURE2D_DESC td{};
        nv12_texture->GetDesc(&td);
        const DWORD total = static_cast<DWORD>((td.Width * td.Height * 3) / 2);
        hr = buffer->SetCurrentLength(total);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"DXGI input buffer SetCurrentLength failed: 0x%08X", hr);
            return false;
        }
    } else {
        // SW path: read back texture to system memory using pre-allocated staging
        D3D11_TEXTURE2D_DESC td{};
        nv12_texture->GetDesc(&td);

        if (!ensure_staging_texture(td.Width, td.Height)) return false;

        d3d_context_->CopyResource(staging_tex_.Get(), nv12_texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3d_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        // NV12: tightly pack Y + UV planes to match stride=width media type.
        const uint32_t tight_stride = td.Width;
        uint32_t y_size  = tight_stride * td.Height;
        uint32_t uv_size = tight_stride * (td.Height / 2);
        DWORD total = y_size + uv_size;

        hr = MFCreateMemoryBuffer(total, &buffer);
        if (FAILED(hr)) {
            d3d_context_->Unmap(staging_tex_.Get(), 0);
            SR_LOG_ERROR(L"MFCreateMemoryBuffer(%u) failed: 0x%08X", total, hr);
            return false;
        }
        BYTE* buf_data = nullptr;
        hr = buffer->Lock(&buf_data, nullptr, nullptr);
        if (FAILED(hr) || !buf_data) {
            if (SUCCEEDED(hr)) buffer->Unlock();
            d3d_context_->Unmap(staging_tex_.Get(), 0);
            SR_LOG_ERROR(L"Software encoder input buffer Lock failed: 0x%08X", hr);
            return false;
        }

        const uint8_t* src = static_cast<uint8_t*>(mapped.pData);
        uint8_t* dst = buf_data;

        // Copy Y plane row-by-row
        for (uint32_t row = 0; row < td.Height; ++row) {
            std::memcpy(dst + row * tight_stride,
                        src + row * mapped.RowPitch,
                        tight_stride);
        }

        // Copy UV plane row-by-row
        const uint8_t* src_uv = src + (mapped.RowPitch * td.Height);
        uint8_t* dst_uv = dst + y_size;
        for (uint32_t row = 0; row < td.Height / 2; ++row) {
            std::memcpy(dst_uv + row * tight_stride,
                        src_uv + row * mapped.RowPitch,
                        tight_stride);
        }

        const HRESULT unlock_hr = buffer->Unlock();
        const HRESULT length_hr = SUCCEEDED(unlock_hr) ? buffer->SetCurrentLength(total) : unlock_hr;

        d3d_context_->Unmap(staging_tex_.Get(), 0);
        if (FAILED(unlock_hr) || FAILED(length_hr)) {
            SR_LOG_ERROR(L"Software encoder input buffer finalize failed: unlock=0x%08X length=0x%08X",
                         unlock_hr, length_hr);
            return false;
        }
    }

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Attaching encoder input buffer failed: 0x%08X", hr);
        return false;
    }
    hr = sample->SetSampleTime(pts);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Setting encoder input timestamp failed: 0x%08X", hr);
        return false;
    }

    // Compute duration (100ns)
    int64_t duration = 10'000'000LL / static_cast<int64_t>(out_fps_);
    hr = sample->SetSampleDuration(duration);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Setting encoder input duration failed: 0x%08X", hr);
        return false;
    }

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

    bool current_input_abandoned = false;
    if (hw_async_mft_) {
        pump_hw_events(0);
        if (hw_need_input_events_ == 0 && hw_have_output_events_ == 0) {
            pump_hw_events(5);
        }
        // Service older queued inputs first so the hardware encoder sees
        // timestamps and frames in capture order.
        submit_deferred_inputs();
        DeferredInput current{ sample, pts, false };
        if (!deferred_inputs_.try_push(std::move(current))) {
            current_input_abandoned = true;
            input_samples_not_accepted_.fetch_add(1, std::memory_order_relaxed);
            pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"Deferred encoder input queue could not retain a frame (capacity=%zu or allocation failure)",
                         kDeferredInputCapacity);
        } else {
            update_deferred_input_metrics();
            submit_deferred_inputs();
            if (auto* queued = deferred_inputs_.find_if([&sample](const DeferredInput& input) {
                    return input.sample.Get() == sample.Get();
                })) {
                if (!queued->was_deferred) {
                    queued->was_deferred = true;
                    input_samples_deferred_without_event_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    } else {
        // Synchronous MFTs retain the conventional ProcessInput/ProcessOutput
        // handshake. A rejected sample is explicitly counted as abandoned.
        hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            input_errors_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"ProcessInput failed: 0x%08X", hr);
            if (recover_stalled_hardware()) {
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false;
        }
        if (SUCCEEDED(hr)) {
            input_samples_accepted_.fetch_add(1, std::memory_order_relaxed);
            pending_video_pts_.push_back(pts);
            pending_input_count_.fetch_add(1, std::memory_order_relaxed);
            uint64_t pending = pending_input_count_.load(std::memory_order_relaxed);
            uint64_t high = pending_input_high_watermark_.load(std::memory_order_relaxed);
            while (pending > high && !pending_input_high_watermark_.compare_exchange_weak(
                       high, pending, std::memory_order_relaxed)) {}
        } else if (hr == MF_E_NOTACCEPTING) {
            input_samples_not_accepted_.fetch_add(1, std::memory_order_relaxed);
            pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"Synchronous MFT rejected an input sample; it cannot be retried after this call");
        }
    }

    if (hw_async_mft_) {
        pump_hw_events(2);
    }

    // Drain available output. Some encoders buffer internally and may not
    // return a sample for every input frame.
    while (true) {
        if (hw_async_mft_ && hw_have_output_events_ == 0) {
            if (recover_stalled_hardware(sample.Get())) {
                if (current_input_abandoned) return false;
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false;
        }

        MFT_OUTPUT_DATA_BUFFER out_buf{};
        DWORD status = 0;

        if (!mft_provides_output_samples_) {
            HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, &out_buf.pSample);
            if (FAILED(alloc_hr)) {
                SR_LOG_ERROR(L"Failed to allocate MFT output sample: 0x%08X", alloc_hr);
                return false;
            }
        }

        hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
        if (hw_async_mft_ && hw_have_output_events_ > 0) {
            --hw_have_output_events_;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            output_waits_for_input_.fetch_add(1, std::memory_order_relaxed);
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            if (hw_async_mft_) {
                pump_hw_events(0);
                submit_deferred_inputs();
            }
            if (recover_stalled_hardware(sample.Get())) {
                if (current_input_abandoned) return false;
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false; // normal: no encoded sample ready yet
        }

        if (FAILED(hr)) {
            LogProcessOutputFailureRateLimited(hr);
            if (hw_path_) {
                ++hw_output_fail_count_;
                if (hr == static_cast<HRESULT>(0x8000FFFF) &&
                    hw_output_fail_count_ >= 10 &&
                    !switched_to_sw_due_to_hw_errors_)
                {
                    remove_deferred_input(sample.Get());
                    if (switch_to_software_fallback()) {
                        switched_to_sw_due_to_hw_errors_ = true;
                        SR_LOG_WARN(L"HW encoder became unstable (0x8000FFFF, 10 consecutive failures). Switched to SW fallback for this session.");
                        if (out_buf.pSample) out_buf.pSample->Release();
                        if (out_buf.pEvents) out_buf.pEvents->Release();
                        if (current_input_abandoned) return false;
                        input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                        return encode_frame(nv12_texture, pts, out_sample, surface_lease);
                    }
                }
            }
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            if (recover_stalled_hardware(sample.Get())) {
                if (current_input_abandoned) return false;
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false;
        }

        if (hw_path_) {
            hw_output_fail_count_ = 0;
        }

        if (out_buf.pSample) {
            if (!stamp_output_sample(out_buf.pSample, pts)) {
                SR_LOG_ERROR(L"Could not assign encoded output timestamps");
            }
            last_output_tick_ms_ = GetTickCount64();
            out_sample.Attach(out_buf.pSample);
            if (out_buf.pEvents) out_buf.pEvents->Release();
            if (hw_async_mft_) {
                pump_hw_events(0);
                submit_deferred_inputs();
            }
            return true;
        }

        if (out_buf.pSample) out_buf.pSample->Release();
        if (out_buf.pEvents) out_buf.pEvents->Release();
    }
}

bool VideoEncoder::switch_to_software_fallback() {
    if (!initialized_ || !mft_) return false;

    const uint32_t width = out_width_;
    const uint32_t height = out_height_;
    const uint32_t fps = out_fps_;

    const uint64_t abandoned = abandon_pending_encoder_inputs(
        pending_video_pts_, pending_input_count_, pending_inputs_abandoned_);
    const uint64_t deferred_abandoned = abandon_deferred_inputs();
    if (abandoned != 0 || deferred_abandoned != 0) {
        SR_LOG_ERROR(L"Encoder fallback abandoned %llu accepted and %llu deferred input frame(s)",
                     static_cast<unsigned long long>(abandoned),
                     static_cast<unsigned long long>(deferred_abandoned));
    }

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mft_.Reset();

    EncoderProfile profile{};
    profile.width = width;
    profile.height = height;
    profile.fps = fps;
    profile.bitrate_bps = active_bitrate_bps_;

    if (!try_init_sw(profile, width, height, fps)) {
        SR_LOG_ERROR(L"Failed to switch to SW fallback after HW encoder instability");
        return false;
    }

    hw_path_ = false;
    hw_output_fail_count_ = 0;
    switched_to_sw_due_to_hw_errors_ = true;
    last_output_tick_ms_ = GetTickCount64();
    publish_configuration();
    return true;
}

void VideoEncoder::publish_configuration() {
    published_width_.store(out_width_, std::memory_order_release);
    published_height_.store(out_height_, std::memory_order_release);
    published_fps_.store(out_fps_, std::memory_order_release);
    published_bitrate_.store(active_bitrate_bps_, std::memory_order_release);
    published_mode_.store(mode_, std::memory_order_release);
}

bool VideoEncoder::stamp_output_sample(IMFSample* sample, int64_t fallback_pts) {
    if (!sample) return false;
    output_samples_produced_.fetch_add(1, std::memory_order_relaxed);

    int64_t output_pts = fallback_pts;
    if (!pending_video_pts_.empty()) {
        output_pts = pending_video_pts_.front();
        pending_video_pts_.pop_front();
        pending_input_count_.fetch_sub(1, std::memory_order_relaxed);
    } else {
        LONGLONG mft_pts = 0;
        if (SUCCEEDED(sample->GetSampleTime(&mft_pts))) {
            output_pts = mft_pts;
        }
    }

    const HRESULT time_hr = sample->SetSampleTime(output_pts);
    const int64_t duration = 10'000'000LL /
        static_cast<int64_t>((std::max)(1u, out_fps_));
    const HRESULT duration_hr = sample->SetSampleDuration(duration);
    if (FAILED(time_hr) || FAILED(duration_hr)) {
        output_timestamp_errors_.fetch_add(1, std::memory_order_relaxed);
        SR_LOG_ERROR(L"Could not restore encoded video timestamps (time=0x%08X, duration=0x%08X)",
                    time_hr, duration_hr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VideoEncoder::flush
// ---------------------------------------------------------------------------
bool VideoEncoder::flush(std::vector<ComPtr<IMFSample>>& out_samples) {
    if (!initialized_ || !mft_) return false;
    bool drain_ok = true;
    try {
        const size_t pending = static_cast<size_t>(pending_input_count_.load(std::memory_order_relaxed)) +
            deferred_inputs_.size();
        if (pending > out_samples.max_size() - out_samples.size()) {
            SR_LOG_ERROR(L"Encoder drain output count exceeds vector capacity limits");
            return false;
        }
        out_samples.reserve(out_samples.size() + pending);
    } catch (...) {
        SR_LOG_ERROR(L"Could not reserve output storage for encoder drain; no more output was requested");
        return false;
    }

    // An async MFT may still have frames waiting for METransformNeedInput.
    // Deliver those frames before END_OF_STREAM; otherwise a clean drain could
    // hide samples the application had captured but never submitted.
    if (hw_async_mft_ && !deferred_inputs_.empty()) {
        const ULONGLONG deferred_deadline = GetTickCount64() + 3000;
        auto collect_one_async_output = [&]() -> bool {
            MFT_OUTPUT_DATA_BUFFER out_buf{};
            DWORD status = 0;
            if (!mft_provides_output_samples_) {
                const HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, &out_buf.pSample);
                if (FAILED(alloc_hr)) {
                    SR_LOG_ERROR(L"Failed to allocate MFT output while submitting deferred inputs: 0x%08X",
                                 alloc_hr);
                    return false;
                }
            }

            const HRESULT output_hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
            if (hw_have_output_events_ > 0) --hw_have_output_events_;
            if (output_hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                output_waits_for_input_.fetch_add(1, std::memory_order_relaxed);
                if (out_buf.pSample) out_buf.pSample->Release();
                if (out_buf.pEvents) out_buf.pEvents->Release();
                return true;
            }
            if (FAILED(output_hr)) {
                SR_LOG_ERROR(L"ProcessOutput while submitting deferred inputs failed: 0x%08X", output_hr);
                if (out_buf.pSample) out_buf.pSample->Release();
                if (out_buf.pEvents) out_buf.pEvents->Release();
                return false;
            }
            if (!out_buf.pSample) {
                SR_LOG_ERROR(L"ProcessOutput completed without a sample for a pending HaveOutput event");
                if (out_buf.pEvents) out_buf.pEvents->Release();
                return false;
            }

            if (!stamp_output_sample(out_buf.pSample, 0)) drain_ok = false;
            ComPtr<IMFSample> sample;
            sample.Attach(out_buf.pSample);
            out_samples.push_back(std::move(sample));
            if (out_buf.pEvents) out_buf.pEvents->Release();
            return true;
        };

        while (!deferred_inputs_.empty() && GetTickCount64() < deferred_deadline) {
            if (!pump_hw_events(10)) drain_ok = false;
            const auto submitted = submit_deferred_inputs();
            if (submitted.failed != 0) drain_ok = false;
            while (hw_have_output_events_ > 0) {
                if (!collect_one_async_output()) {
                    drain_ok = false;
                    break;
                }
                if (!pump_hw_events(0)) drain_ok = false;
                const auto retried = submit_deferred_inputs();
                if (retried.failed != 0) drain_ok = false;
            }
        }

        if (!deferred_inputs_.empty()) {
            const uint64_t abandoned = abandon_deferred_inputs();
            drain_ok = false;
            SR_LOG_ERROR(L"Async MFT drain could not submit %llu deferred input frame(s) before timeout",
                         static_cast<unsigned long long>(abandoned));
        }
    }

    // Some MFTs only emit buffered output after explicit end-of-stream signal.
    HRESULT eos_hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(eos_hr)) {
        SR_LOG_ERROR(L"MFT_MESSAGE_NOTIFY_END_OF_STREAM failed: 0x%08X", eos_hr);
        return false;
    }

    const HRESULT drain_hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(drain_hr)) {
        SR_LOG_ERROR(L"MFT_MESSAGE_COMMAND_DRAIN failed: 0x%08X", drain_hr);
        return false;
    }

    if (hw_async_mft_) {
        hw_drain_complete_ = false;
        const ULONGLONG deadline = GetTickCount64() + 3000;

        while ((!hw_drain_complete_ || hw_have_output_events_ > 0) && GetTickCount64() < deadline) {
            if (!pump_hw_events(10)) drain_ok = false;

            while (hw_have_output_events_ > 0) {
                MFT_OUTPUT_DATA_BUFFER out_buf{};
                DWORD status = 0;

                if (!mft_provides_output_samples_) {
                    HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, &out_buf.pSample);
                    if (FAILED(alloc_hr)) {
                        SR_LOG_ERROR(L"Failed to allocate async MFT flush output sample: 0x%08X", alloc_hr);
                        return false;
                    }
                }

                HRESULT hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
                if (hw_have_output_events_ > 0) {
                    --hw_have_output_events_;
                }

                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    if (out_buf.pSample) out_buf.pSample->Release();
                    if (out_buf.pEvents) out_buf.pEvents->Release();
                    break;
                }
                if (FAILED(hr)) {
                    SR_LOG_ERROR(L"Async ProcessOutput during flush failed: 0x%08X", hr);
                    if (out_buf.pSample) out_buf.pSample->Release();
                    if (out_buf.pEvents) out_buf.pEvents->Release();
                    return false;
                }

                if (out_buf.pSample) {
                    if (!stamp_output_sample(out_buf.pSample, 0)) drain_ok = false;
                    ComPtr<IMFSample> s;
                    s.Attach(out_buf.pSample);
                    out_samples.push_back(std::move(s));
                }
                if (out_buf.pEvents) out_buf.pEvents->Release();
            }
        }

        const bool timed_out = !hw_drain_complete_ && GetTickCount64() >= deadline;
        if (!hw_drain_complete_) {
            SR_LOG_ERROR(L"Async MFT drain timed out with %llu pending input timestamp(s)",
                         static_cast<unsigned long long>(pending_input_count_.load(std::memory_order_relaxed)));
            drain_ok = false;
        }
        if (hw_have_output_events_ > 0) {
            SR_LOG_ERROR(L"Async MFT drain completed with %u unprocessed output event(s)",
                         hw_have_output_events_);
            drain_ok = false;
        }
        return encoder_drain_succeeded(hw_drain_complete_, timed_out, drain_ok,
            pending_input_count_.load(std::memory_order_relaxed));
    }

    while (true) {
        MFT_OUTPUT_DATA_BUFFER out_buf{};
        DWORD status = 0;

        if (!mft_provides_output_samples_) {
            HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, &out_buf.pSample);
            if (FAILED(alloc_hr)) {
                SR_LOG_ERROR(L"Failed to allocate MFT flush output sample: 0x%08X", alloc_hr);
                return false;
            }
        }

        HRESULT hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            break;
        }
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"ProcessOutput during flush failed: 0x%08X", hr);
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            return false;
        }
        if (out_buf.pSample) {
            if (!stamp_output_sample(out_buf.pSample, 0)) drain_ok = false;
            ComPtr<IMFSample> s;
            s.Attach(out_buf.pSample);
            out_samples.push_back(std::move(s));
        }
        if (out_buf.pEvents) out_buf.pEvents->Release();
        if (!out_buf.pSample) {
            SR_LOG_ERROR(L"ProcessOutput succeeded during drain without returning a sample");
            return false;
        }
    }
    if (!pending_video_pts_.empty()) {
        SR_LOG_ERROR(L"Synchronous MFT drain left %llu pending input timestamp(s)",
                     static_cast<unsigned long long>(pending_input_count_.load(std::memory_order_relaxed)));
        drain_ok = false;
    }
    return encoder_drain_succeeded(true, false, drain_ok,
        pending_input_count_.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// VideoEncoder::ensure_staging_texture — lazy-init or resize staging texture
// ---------------------------------------------------------------------------
bool VideoEncoder::ensure_staging_texture(uint32_t width, uint32_t height) {
    if (staging_tex_ && staging_width_ == width && staging_height_ == height) {
        return true;  // already correct size
    }
    staging_tex_.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = width;
    td.Height           = height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.BindFlags        = 0;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    td.MiscFlags        = 0;

    HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &staging_tex_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"ensure_staging_texture: CreateTexture2D(%ux%u) failed: 0x%08X", width, height, hr);
        return false;
    }
    staging_width_  = width;
    staging_height_ = height;
    SR_LOG_INFO(L"Staging texture allocated: %ux%u NV12", width, height);
    return true;
}

// ---------------------------------------------------------------------------
// VideoEncoder::shutdown
// ---------------------------------------------------------------------------
void VideoEncoder::shutdown() {
    if (!deferred_inputs_.empty()) {
        const uint64_t abandoned = abandon_deferred_inputs();
        SR_LOG_ERROR(L"Encoder shutdown released %llu unsent deferred input frame(s)",
                     static_cast<unsigned long long>(abandoned));
    }
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_.Reset();
    }
    hw_events_.Reset();
    staging_tex_.Reset();
    staging_width_  = 0;
    staging_height_ = 0;
    initialized_ = false;
    hw_async_mft_ = false;
    hw_need_input_events_ = 0;
    hw_have_output_events_ = 0;
    hw_drain_complete_ = false;
}

} // namespace sr
