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
#include <algorithm>
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

static HRESULT CreateMftOutputSample(uint32_t sample_size, uint32_t alignment,
                                     IMFSample** sample_out) {
    if (!sample_out) return E_POINTER;
    *sample_out = nullptr;

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    hr = MFCreateAlignedMemoryBuffer(sample_size, alignment, &buffer);
    if (FAILED(hr)) return hr;

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;

    *sample_out = sample.Detach();
    return S_OK;
}

static HRESULT GetSampleBufferCapacity(IMFSample* sample, DWORD* length_out) {
    // The async output queue is RAM-bounded, so charge allocated buffer
    // capacity rather than only the compressed payload's current length.
    if (!sample || !length_out) return E_POINTER;
    *length_out = 0;
    DWORD buffer_count = 0;
    HRESULT hr = sample->GetBufferCount(&buffer_count);
    if (FAILED(hr)) return hr;
    ULONGLONG total = 0;
    for (DWORD i = 0; i < buffer_count; ++i) {
        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->GetBufferByIndex(i, &buffer);
        if (FAILED(hr)) return hr;
        DWORD buffer_capacity = 0;
        hr = buffer->GetMaxLength(&buffer_capacity);
        if (FAILED(hr)) return hr;
        total += buffer_capacity;
        if (total > MAXDWORD) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    *length_out = static_cast<DWORD>(total);
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
    hw_event_callback_.Reset();
    hw_shutdown_.Reset();
    hw_events_.Reset();
    hw_async_mft_ = false;

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

        // Isolate events from a rejected candidate. A late callback retains
        // this generation's scheduler and cannot mutate the next candidate.
        try {
            async_scheduler_ = std::make_shared<AsyncScheduler>(
                kAsyncMftInputQueueCapacity, kAsyncMftMaxInFlightInputs,
                kAsyncMftOutputQueueCapacity, kAsyncMftOutputQueueByteCapacity);
        } catch (...) {
            SR_LOG_ERROR(L"Could not allocate asynchronous MFT scheduler");
            activates[i]->ShutdownObject();
            continue;
        }

        // Hardware MFTs are asynchronous by contract. Unlock them before
        // stream configuration and require both documented async interfaces.
        ComPtr<IMFAttributes> attrs;
        hr = mft->GetAttributes(&attrs);
        UINT32 is_async = FALSE;
        if (FAILED(hr) || !attrs || FAILED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) || !is_async) {
            SR_LOG_WARN(L"HW MFT %u does not expose the required asynchronous attribute", i);
            activates[i]->ShutdownObject();
            continue;
        }
        hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT %u async unlock failed (0x%08X)", i, hr);
            activates[i]->ShutdownObject();
            continue;
        }

        ComPtr<IMFMediaEventGenerator> events;
        ComPtr<IMFShutdown> shutdown;
        hr = mft.As(&events);
        if (SUCCEEDED(hr)) hr = mft.As(&shutdown);
        if (FAILED(hr) || !events || !shutdown) {
            SR_LOG_WARN(L"HW MFT %u lacks IMFMediaEventGenerator or IMFShutdown (0x%08X)", i, hr);
            activates[i]->ShutdownObject();
            continue;
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

        MFT_OUTPUT_STREAM_INFO output_stream_info{};
        hr = mft->GetOutputStreamInfo(0, &output_stream_info);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': GetOutputStreamInfo failed (0x%08X)", name, hr);
            activates[i]->ShutdownObject();
            continue;
        }
        const bool provides_output_samples =
            (output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        const uint32_t output_sample_size = output_stream_info.cbSize > 0
            ? output_stream_info.cbSize : (1 << 20);
        const uint32_t output_sample_alignment = output_stream_info.cbAlignment != 0
            ? output_stream_info.cbAlignment : MF_1_BYTE_ALIGNMENT;

        hr = mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': stream initialization message failed (0x%08X)", name, hr);
            activates[i]->ShutdownObject();
            continue;
        }

        auto callback = Microsoft::WRL::Make<MftAsyncEventCallback<AsyncScheduler>>();
        if (!callback) {
            SR_LOG_ERROR(L"Could not allocate asynchronous MFT event callback");
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            activates[i]->ShutdownObject();
            continue;
        }
        hr = callback->start(events.Get(), async_scheduler_);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': BeginGetEvent failed (0x%08X)", name, hr);
            callback->disarm();
            shutdown->Shutdown();
            callback->wait_quiescent(3000);
            activates[i]->ShutdownObject();
            continue;
        }
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"HW MFT '%s': START_OF_STREAM failed (0x%08X)", name, hr);
            callback->disarm();
            shutdown->Shutdown();
            callback->wait_quiescent(3000);
            activates[i]->ShutdownObject();
            continue;
        }

        mft_ = mft;
        hw_events_ = events;
        hw_shutdown_ = shutdown;
        hw_event_callback_ = callback;
        mode_ = EncoderMode::HardwareMFT;
        out_width_ = profile.width;
        out_height_ = profile.height;
        out_fps_ = profile.fps;
        hw_path_ = true;
        hw_async_mft_ = true;
        mft_provides_output_samples_ = provides_output_samples;
        mft_output_sample_size_ = output_sample_size;
        mft_output_sample_alignment_ = output_sample_alignment;
        last_output_tick_ms_ = GetTickCount64();

        if (!start_async_worker()) {
            SR_LOG_ERROR(L"HW MFT '%s': could not start the serialized encoder worker", name);
            callback->disarm();
            shutdown->Shutdown();
            callback->wait_quiescent(3000);
            mft_.Reset();
            hw_events_.Reset();
            hw_shutdown_.Reset();
            hw_event_callback_.Reset();
            hw_async_mft_ = false;
            activates[i]->ShutdownObject();
            continue;
        }

        SR_LOG_INFO(L"HW H.264 async encoder active: %s (%ux%u @ %u fps, %u bps)",
            name, profile.width, profile.height, profile.fps, profile.bitrate_bps);

        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return true;
    }

    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return false;
}

bool VideoEncoder::start_async_worker() {
    if (!hw_async_mft_ || !mft_ || !hw_events_ || !hw_event_callback_) return false;
    async_worker_stopping_.store(false, std::memory_order_release);
    try {
        async_worker_ = std::thread(&VideoEncoder::async_worker_loop, this);
    } catch (...) {
        async_worker_stopping_.store(true, std::memory_order_release);
        scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool VideoEncoder::try_pop_async_output(AsyncEncodedOutput& output) {
    if (!hw_async_mft_) return false;
    auto next = async_scheduler_->try_pop_output();
    if (!next) return false;
    output = std::move(*next);
    return true;
}

void VideoEncoder::notify_pipeline(PipelineWakeSource source) {
    if (auto signal = pipeline_wake_signal_.lock()) signal->notify(source);
}

void VideoEncoder::async_worker_loop() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_hr);
    struct ComApartmentGuard {
        bool initialized;
        ~ComApartmentGuard() { if (initialized) CoUninitialize(); }
    } com_guard{ com_initialized };
    if (FAILED(com_hr)) {
        scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
        async_scheduler_->fail(com_hr);
    }

    std::optional<AsyncEncodedOutput> blocked_output;
    AsyncMftStallWatchdog watchdog(std::chrono::milliseconds(5'000));

    auto record_sample_time = [](IMFSample* sample, int64_t pts, uint32_t fps) {
        if (!sample) return E_POINTER;
        const HRESULT time_hr = sample->SetSampleTime(pts);
        const HRESULT duration_hr = sample->SetSampleDuration(
            10'000'000LL / static_cast<int64_t>((std::max)(1u, fps)));
        return FAILED(time_hr) ? time_hr : duration_hr;
    };

    auto publish_blocked_output = [&]() -> bool {
        if (!blocked_output) return true;
        DWORD bytes = 0;
        HRESULT hr = GetSampleBufferCapacity(blocked_output->sample.Get(), &bytes);
        if (FAILED(hr)) {
            output_errors_.fetch_add(1, std::memory_order_relaxed);
            async_scheduler_->fail(hr);
            blocked_output.reset();
            return false;
        }
        if (bytes == 0) {
            output_errors_.fetch_add(1, std::memory_order_relaxed);
            async_scheduler_->fail(E_UNEXPECTED);
            blocked_output.reset();
            return false;
        }
        if (async_scheduler_->publish_output(*blocked_output, bytes)) {
            blocked_output.reset();
            watchdog.mark_progress(AsyncMftStallWatchdog::Clock::now());
            notify_pipeline(PipelineWakeSource::EncoderOutput);
            return true;
        }
        if (async_scheduler_->failed()) {
            return false;
        }
        return true; // Keep the sample locally; output queue is applying backpressure.
    };

    while (SUCCEEDED(com_hr) && !async_worker_stopping_.load(std::memory_order_acquire)) {
        bool progressed = false;
        if (async_scheduler_->failed()) break;

        if (blocked_output) {
            const auto wait_started = std::chrono::steady_clock::now();
            if (!publish_blocked_output()) break;
            if (blocked_output) {
                DWORD output_bytes = 0;
                const HRESULT length_hr = GetSampleBufferCapacity(blocked_output->sample.Get(), &output_bytes);
                if (FAILED(length_hr) || output_bytes == 0) {
                    async_scheduler_->fail(FAILED(length_hr) ? length_hr : E_UNEXPECTED);
                    break;
                }
                const bool woke = [&] {
                    async_scheduler_->wait_for_output_space(output_bytes);
                    return !async_worker_stopping_.load(std::memory_order_acquire) &&
                           !async_scheduler_->failed() && !async_scheduler_->stopped();
                }();
                async_event_wait_us_.record(static_cast<uint64_t>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_started).count()));
                async_worker_wakeups_.fetch_add(woke ? 1 : 0, std::memory_order_relaxed);
                continue;
            }
            progressed = true;
        }

        if (async_scheduler_->output_ready() && async_scheduler_->output_has_capacity() &&
            async_scheduler_->take_output_event()) {
            MFT_OUTPUT_DATA_BUFFER output_buffer{};
            DWORD output_status = 0;
            if (!mft_provides_output_samples_) {
                const HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, mft_output_sample_alignment_, &output_buffer.pSample);
                if (FAILED(alloc_hr)) {
                    output_errors_.fetch_add(1, std::memory_order_relaxed);
                    async_scheduler_->fail(alloc_hr);
                    break;
                }
            }

            const auto process_started = std::chrono::steady_clock::now();
            const HRESULT output_hr = mft_->ProcessOutput(0, 1, &output_buffer, &output_status);
            async_process_output_us_.record(static_cast<uint64_t>(std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() - process_started).count()));
            if (SUCCEEDED(output_hr) &&
                ((output_status & MFT_PROCESS_OUTPUT_STATUS_NEW_STREAMS) != 0 ||
                 (output_buffer.dwStatus & (MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE |
                                            MFT_OUTPUT_DATA_BUFFER_STREAM_END |
                                            MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) != 0)) {
                SR_LOG_ERROR(L"Async encoder reported unsupported output stream change/status: process=0x%08X output=0x%08X",
                             output_status, output_buffer.dwStatus);
                if (output_buffer.pSample) output_buffer.pSample->Release();
                if (output_buffer.pEvents) output_buffer.pEvents->Release();
                output_errors_.fetch_add(1, std::memory_order_relaxed);
                async_scheduler_->fail(MF_E_TRANSFORM_STREAM_CHANGE);
                break;
            }
            if (output_hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                output_waits_for_input_.fetch_add(1, std::memory_order_relaxed);
                if (output_buffer.pSample) output_buffer.pSample->Release();
                if (output_buffer.pEvents) output_buffer.pEvents->Release();
                output_errors_.fetch_add(1, std::memory_order_relaxed);
                async_scheduler_->fail(E_UNEXPECTED); // Async MFTs signal readiness with NeedInput.
                break;
            }
            if (FAILED(output_hr) || !output_buffer.pSample) {
                if (output_buffer.pSample) output_buffer.pSample->Release();
                if (output_buffer.pEvents) output_buffer.pEvents->Release();
                output_errors_.fetch_add(1, std::memory_order_relaxed);
                async_scheduler_->fail(FAILED(output_hr) ? output_hr : E_UNEXPECTED);
                break;
            }

            AsyncEncodedOutput encoded{};
            encoded.sample.Attach(output_buffer.pSample);
            if (output_buffer.pEvents) output_buffer.pEvents->Release();
            LONGLONG output_pts = 0;
            const HRESULT output_time_hr = encoded.sample->GetSampleTime(&output_pts);
            auto metadata_it = async_pending_frames_.begin();
            if (SUCCEEDED(output_time_hr)) {
                metadata_it = std::find_if(async_pending_frames_.begin(), async_pending_frames_.end(),
                    [output_pts](const AcceptedFrameMetadata& metadata) {
                        return metadata.pts == output_pts;
                    });
            }
            if ((FAILED(output_time_hr) && output_time_hr != MF_E_NO_SAMPLE_TIMESTAMP) ||
                async_pending_frames_.empty() ||
                (SUCCEEDED(output_time_hr) && metadata_it == async_pending_frames_.end())) {
                output_timestamp_errors_.fetch_add(1, std::memory_order_relaxed);
                SR_LOG_ERROR(L"Async encoder output timestamp did not match an accepted input (hr=0x%08X pts=%lld)",
                             output_time_hr, output_pts);
                async_scheduler_->fail(E_UNEXPECTED);
            } else {
                // Match by the transform-preserved PTS when available. This
                // keeps frame IDs correct even if an MFT reorders output;
                // FIFO remains the fallback for transforms that omit PTS.
                const auto metadata = *metadata_it;
                async_pending_frames_.erase(metadata_it);
                encoded.frame_id = metadata.frame_id;
                encoded.pts = metadata.pts;
                encoded.synthetic = metadata.synthetic;
                const auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - metadata.accepted_at).count();
                async_accept_to_output_us_.record(static_cast<uint64_t>((std::max)(0LL, latency_us)));
                const HRESULT timestamp_hr = record_sample_time(encoded.sample.Get(), metadata.pts, out_fps_);
                if (FAILED(timestamp_hr)) {
                    output_timestamp_errors_.fetch_add(1, std::memory_order_relaxed);
                    async_scheduler_->fail(timestamp_hr);
                }
                pending_input_count_.fetch_sub(1, std::memory_order_relaxed);
                async_scheduler_->mark_output_produced();
            }
            if (async_scheduler_->failed()) break;
            output_samples_produced_.fetch_add(1, std::memory_order_relaxed);
            last_output_tick_ms_ = GetTickCount64();
            blocked_output.emplace(std::move(encoded));
            if (!publish_blocked_output() || async_scheduler_->failed()) break;
            progressed = true;
        }

        if (async_scheduler_->input_ready()) {
            auto request = async_scheduler_->take_input_request();
            if (request) {
                DeferredInput input = std::move(request->input);
                const auto residence_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - input.queued_at).count();
                async_input_queue_residence_us_.record(static_cast<uint64_t>((std::max)(0LL, residence_us)));
                if (input.force_keyframe) {
                    ComPtr<ICodecAPI> codec_api;
                    const HRESULT query_hr = mft_->QueryInterface(IID_PPV_ARGS(&codec_api));
                    if (SUCCEEDED(query_hr)) {
                        VARIANT value{};
                        value.vt = VT_UI4;
                        value.ulVal = 1;
                        const HRESULT keyframe_hr = codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
                        if (FAILED(keyframe_hr)) SR_LOG_WARN(L"Forcing encoder keyframe failed: 0x%08X", keyframe_hr);
                    }
                }

                const auto process_started = std::chrono::steady_clock::now();
                const HRESULT input_hr = mft_->ProcessInput(request->stream_id, input.sample.Get(), 0);
                async_process_input_us_.record(static_cast<uint64_t>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - process_started).count()));
                if (SUCCEEDED(input_hr)) {
                    input_samples_accepted_.fetch_add(1, std::memory_order_relaxed);
                    if (input.was_deferred) {
                        input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                    }
                    const uint64_t pending = pending_input_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                    uint64_t high = pending_input_high_watermark_.load(std::memory_order_relaxed);
                    while (pending > high && !pending_input_high_watermark_.compare_exchange_weak(
                               high, pending, std::memory_order_relaxed)) {}
                    async_scheduler_->mark_input_accepted();
                    try {
                        async_pending_frames_.push_back({ input.frame_id, input.pts, input.synthetic,
                                                          std::chrono::steady_clock::now() });
                    } catch (...) {
                        // ProcessInput accepted the sample. Preserve truthful
                        // accounting even if the metadata ledger cannot grow.
                        input_accounting_errors_.fetch_add(1, std::memory_order_relaxed);
                        pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
                        pending_input_count_.fetch_sub(1, std::memory_order_relaxed);
                        async_scheduler_->fail(E_OUTOFMEMORY);
                        break;
                    }
                    watchdog.mark_progress(AsyncMftStallWatchdog::Clock::now());
                } else if (input_hr == MF_E_NOTACCEPTING) {
                    input_samples_not_accepted_.fetch_add(1, std::memory_order_relaxed);
                    if (!input.was_deferred) {
                        input.was_deferred = true;
                        input_samples_deferred_without_event_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!async_scheduler_->retry_input_front(std::move(input))) {
                        input_accounting_errors_.fetch_add(1, std::memory_order_relaxed);
                        async_scheduler_->fail(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
                    }
                } else {
                    input_errors_.fetch_add(1, std::memory_order_relaxed);
                    pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
                    async_scheduler_->fail(input_hr);
                }
                progressed = true;
            }
        }

        if (async_scheduler_->take_drain_command()) {
            HRESULT drain_hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            if (SUCCEEDED(drain_hr)) drain_hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(drain_hr)) {
                scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
                async_scheduler_->fail(drain_hr);
            }
            progressed = true;
        }

        if (async_scheduler_->drain_complete() && async_scheduler_->output_events_pending() == 0 &&
            !blocked_output && async_pending_frames_.empty()) {
            async_scheduler_->signal_worker_drained();
            break;
        }
        if (async_scheduler_->drain_complete() && async_scheduler_->output_events_pending() == 0 &&
            !blocked_output && !async_pending_frames_.empty()) {
            input_accounting_errors_.fetch_add(1, std::memory_order_relaxed);
            async_scheduler_->fail(E_FAIL);
            break;
        }
        if (progressed) continue;

        const auto wait_started = std::chrono::steady_clock::now();
        const bool has_pending_work =
            pending_input_count_.load(std::memory_order_relaxed) != 0 ||
            async_scheduler_->input_count() != 0;
        bool woke = true;
        const auto watchdog_wait = watchdog.until_stalled(
            has_pending_work, AsyncMftStallWatchdog::Clock::now());
        if (watchdog_wait) {
            woke = async_scheduler_->wait_for_work(*watchdog_wait);
        } else {
            async_scheduler_->wait_for_work();
        }
        async_event_wait_us_.record(static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_started).count()));
        if (woke) async_worker_wakeups_.fetch_add(1, std::memory_order_relaxed);
        if (!woke && watchdog_wait) {
            scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
            async_scheduler_->fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
    }

    if (async_scheduler_->failed()) {
        const uint64_t abandoned = static_cast<uint64_t>(async_pending_frames_.size());
        if (abandoned != 0) {
            pending_inputs_abandoned_.fetch_add(abandoned, std::memory_order_relaxed);
            pending_input_count_.fetch_sub(abandoned, std::memory_order_relaxed);
            async_pending_frames_.clear();
        }
        const uint64_t queued = static_cast<uint64_t>(async_scheduler_->abandon_queued_inputs());
        if (queued != 0) pending_inputs_abandoned_.fetch_add(queued, std::memory_order_relaxed);
        if (blocked_output) {
            output_errors_.fetch_add(1, std::memory_order_relaxed);
            blocked_output.reset();
        }
    }
    async_scheduler_->signal_worker_drained();
    if (async_scheduler_->failed()) notify_pipeline(PipelineWakeSource::EncoderFailure);
}

// ---------------------------------------------------------------------------
// VideoEncoder::try_init_sw
// ---------------------------------------------------------------------------
bool VideoEncoder::try_init_sw(const EncoderProfile& profile,
                                uint32_t width, uint32_t height, uint32_t fps)
{
    hw_events_.Reset();
    hw_async_mft_ = false;

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
            mft_output_sample_alignment_ = output_stream_info.cbAlignment != 0
                ? output_stream_info.cbAlignment : MF_1_BYTE_ALIGNMENT;
        } else {
            mft_provides_output_samples_ = true;
            mft_output_sample_size_ = 1 << 20;
            mft_output_sample_alignment_ = MF_1_BYTE_ALIGNMENT;
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
    try {
        // Never reset a scheduler that a timed-out callback may still own.
        async_scheduler_ = std::make_shared<AsyncScheduler>(
            kAsyncMftInputQueueCapacity, kAsyncMftMaxInFlightInputs,
            kAsyncMftOutputQueueCapacity, kAsyncMftOutputQueueByteCapacity);
    } catch (...) {
        SR_LOG_ERROR(L"Could not allocate asynchronous encoder scheduler");
        return false;
    }
    input_samples_offered_.store(0, std::memory_order_relaxed);
    input_samples_accepted_.store(0, std::memory_order_relaxed);
    input_samples_not_accepted_.store(0, std::memory_order_relaxed);
    input_queue_backpressure_waits_.store(0, std::memory_order_relaxed);
    input_queue_backpressure_timeouts_.store(0, std::memory_order_relaxed);
    input_samples_deferred_without_event_.store(0, std::memory_order_relaxed);
    input_errors_.store(0, std::memory_order_relaxed);
    output_samples_produced_.store(0, std::memory_order_relaxed);
    output_waits_for_input_.store(0, std::memory_order_relaxed);
    pending_input_count_.store(0, std::memory_order_relaxed);
    pending_input_high_watermark_.store(0, std::memory_order_relaxed);
    pending_inputs_abandoned_.store(0, std::memory_order_relaxed);
    input_samples_retried_.store(0, std::memory_order_relaxed);
    output_timestamp_errors_.store(0, std::memory_order_relaxed);
    input_preparation_errors_.store(0, std::memory_order_relaxed);
    output_errors_.store(0, std::memory_order_relaxed);
    scheduler_errors_.store(0, std::memory_order_relaxed);
    input_accounting_errors_.store(0, std::memory_order_relaxed);
    pending_video_pts_.clear();
    async_pending_frames_.clear();
    async_worker_wakeups_.store(0, std::memory_order_relaxed);
    next_frame_id_.store(0, std::memory_order_relaxed);
    async_event_wait_us_.reset();
    async_input_queue_residence_us_.reset();
    async_process_input_us_.reset();
    async_process_output_us_.reset();
    async_accept_to_output_us_.reset();

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
                                 std::shared_ptr<TextureSurfaceLease> surface_lease,
                                 bool synthetic)
{
    if (!initialized_ || !mft_) {
        if (initialized_) input_preparation_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint64_t frame_id = next_frame_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    input_samples_offered_.fetch_add(1, std::memory_order_relaxed);
    if (hw_async_mft_ && async_scheduler_->failed()) {
        input_samples_not_accepted_.fetch_add(1, std::memory_order_relaxed);
        pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto fail_preparation = [this]() {
        input_preparation_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!nv12_texture || (!hw_path_ && !d3d_context_)) return fail_preparation();

    auto recover_stalled_hardware = [&](IMFSample* retry_sample = nullptr) {
        constexpr ULONGLONG kOutputStallLimitMs = 5'000;
        // The asynchronous MFT owns accepted frames on a dedicated worker.
        // Replacing it from the submission thread would race ProcessInput/
        // ProcessOutput and could discard accepted output; fail this frame and
        // let the worker watchdog report a stalled transform instead.
        if (!hw_path_ || hw_async_mft_ || switched_to_sw_due_to_hw_errors_ ||
            last_output_tick_ms_ == 0 ||
            GetTickCount64() - last_output_tick_ms_ < kOutputStallLimitMs) {
            return false;
        }
        SR_LOG_ERROR(L"Hardware encoder produced no output for %llu ms; switching to software fallback",
                     GetTickCount64() - last_output_tick_ms_);
        (void)retry_sample;
        return switch_to_software_fallback();
    };

    // Create an IMFSample wrapping the NV12 texture
    ComPtr<IMFSample>      sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = hw_path_
        ? create_tracked_surface_sample(surface_lease, &sample)
        : MFCreateSample(&sample);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Creating encoder input sample failed: 0x%08X", hr);
        return fail_preparation();
    }

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
                return encode_frame(nv12_texture, pts, out_sample, surface_lease, synthetic);
            }
            return fail_preparation();
        }

        D3D11_TEXTURE2D_DESC td{};
        nv12_texture->GetDesc(&td);
        const DWORD total = static_cast<DWORD>((td.Width * td.Height * 3) / 2);
        hr = buffer->SetCurrentLength(total);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"DXGI input buffer SetCurrentLength failed: 0x%08X", hr);
            return fail_preparation();
        }
    } else {
        // SW path: read back texture to system memory using pre-allocated staging
        D3D11_TEXTURE2D_DESC td{};
        nv12_texture->GetDesc(&td);

        if (!ensure_staging_texture(td.Width, td.Height)) return fail_preparation();

        d3d_context_->CopyResource(staging_tex_.Get(), nv12_texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3d_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return fail_preparation();

        // NV12: tightly pack Y + UV planes to match stride=width media type.
        const uint32_t tight_stride = td.Width;
        uint32_t y_size  = tight_stride * td.Height;
        uint32_t uv_size = tight_stride * (td.Height / 2);
        DWORD total = y_size + uv_size;

        hr = MFCreateMemoryBuffer(total, &buffer);
        if (FAILED(hr)) {
            d3d_context_->Unmap(staging_tex_.Get(), 0);
            SR_LOG_ERROR(L"MFCreateMemoryBuffer(%u) failed: 0x%08X", total, hr);
            return fail_preparation();
        }
        BYTE* buf_data = nullptr;
        hr = buffer->Lock(&buf_data, nullptr, nullptr);
        if (FAILED(hr) || !buf_data) {
            if (SUCCEEDED(hr)) buffer->Unlock();
            d3d_context_->Unmap(staging_tex_.Get(), 0);
            SR_LOG_ERROR(L"Software encoder input buffer Lock failed: 0x%08X", hr);
            return fail_preparation();
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
            return fail_preparation();
        }
    }

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Attaching encoder input buffer failed: 0x%08X", hr);
        return fail_preparation();
    }
    hr = sample->SetSampleTime(pts);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Setting encoder input timestamp failed: 0x%08X", hr);
        return fail_preparation();
    }

    // Compute duration (100ns)
    int64_t duration = 10'000'000LL / static_cast<int64_t>(out_fps_);
    hr = sample->SetSampleDuration(duration);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"Setting encoder input duration failed: 0x%08X", hr);
        return fail_preparation();
    }

    // The asynchronous MFT is worker-owned from this point onward. Keep the
    // prepared sample and surface lease until a matching NeedInput credit arrives.
    const bool force_keyframe = force_keyframe_next_.exchange(false, std::memory_order_acq_rel);
    if (hw_async_mft_) {
        DeferredInput queued{};
        queued.sample = std::move(sample);
        queued.pts = pts;
        queued.frame_id = frame_id;
        queued.synthetic = synthetic;
        queued.force_keyframe = force_keyframe;
        queued.queued_at = std::chrono::steady_clock::now();
        bool was_backpressured = false;
        if (!async_scheduler_->enqueue_input_wait(
                std::move(queued), std::chrono::seconds(5), &was_backpressured)) {
            if (was_backpressured &&
                !async_scheduler_->failed() && !async_scheduler_->stopped()) {
                input_queue_backpressure_timeouts_.fetch_add(1, std::memory_order_relaxed);
                async_scheduler_->fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            }
            pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"Async encoder could not enqueue frame %llu (backpressure_waited=%u failed=%u stopped=%u)",
                         static_cast<unsigned long long>(frame_id), was_backpressured ? 1u : 0u,
                         async_scheduler_->failed() ? 1u : 0u,
                         async_scheduler_->stopped() ? 1u : 0u);
            return false;
        }
        if (was_backpressured)
            input_queue_backpressure_waits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Force IDR keyframe if requested (e.g. after resume from pause)
    if (force_keyframe) {
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(mft_->QueryInterface(IID_PPV_ARGS(&codec_api)))) {
            VARIANT v{};
            v.vt   = VT_UI4;
            v.ulVal = 1;  // 1 = force next frame as IDR
            codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
            SR_LOG_INFO(L"VideoEncoder: forced IDR keyframe on resume");
        }
    }

    {
        // Synchronous MFTs retain the conventional ProcessInput/ProcessOutput
        // handshake. A rejected sample is explicitly counted as abandoned.
        hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            input_errors_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"ProcessInput failed: 0x%08X", hr);
            if (recover_stalled_hardware()) {
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease, synthetic);
            }
            return false;
        }
        if (SUCCEEDED(hr)) {
            input_samples_accepted_.fetch_add(1, std::memory_order_relaxed);
            try {
                pending_video_pts_.push_back(pts);
            } catch (...) {
                input_accounting_errors_.fetch_add(1, std::memory_order_relaxed);
                pending_inputs_abandoned_.fetch_add(1, std::memory_order_relaxed);
                SR_LOG_ERROR(L"Could not allocate encoder timestamp ledger entry after input acceptance");
                return false;
            }
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

    // Drain available output. Some encoders buffer internally and may not
    // return a sample for every input frame.
    while (true) {
        MFT_OUTPUT_DATA_BUFFER out_buf{};
        DWORD status = 0;

        if (!mft_provides_output_samples_) {
            HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, mft_output_sample_alignment_, &out_buf.pSample);
            if (FAILED(alloc_hr)) {
                SR_LOG_ERROR(L"Failed to allocate MFT output sample: 0x%08X", alloc_hr);
                output_errors_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        hr = mft_->ProcessOutput(0, 1, &out_buf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            output_waits_for_input_.fetch_add(1, std::memory_order_relaxed);
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            if (recover_stalled_hardware(sample.Get())) {
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease);
            }
            return false; // normal: no encoded sample ready yet
        }

        if (FAILED(hr)) {
            output_errors_.fetch_add(1, std::memory_order_relaxed);
            LogProcessOutputFailureRateLimited(hr);
            if (hw_path_) {
                ++hw_output_fail_count_;
                if (hr == static_cast<HRESULT>(0x8000FFFF) &&
                    hw_output_fail_count_ >= 10 &&
                    !switched_to_sw_due_to_hw_errors_)
                {
                    if (switch_to_software_fallback()) {
                        switched_to_sw_due_to_hw_errors_ = true;
                        SR_LOG_WARN(L"HW encoder became unstable (0x8000FFFF, 10 consecutive failures). Switched to SW fallback for this session.");
                        if (out_buf.pSample) out_buf.pSample->Release();
                        if (out_buf.pEvents) out_buf.pEvents->Release();
                        input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                        return encode_frame(nv12_texture, pts, out_sample, surface_lease, synthetic);
                    }
                }
            }
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            if (recover_stalled_hardware(sample.Get())) {
                input_samples_retried_.fetch_add(1, std::memory_order_relaxed);
                return encode_frame(nv12_texture, pts, out_sample, surface_lease, synthetic);
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
            return true;
        }

        if (out_buf.pSample) out_buf.pSample->Release();
        if (out_buf.pEvents) out_buf.pEvents->Release();
    }
}

bool VideoEncoder::switch_to_software_fallback() {
    if (!initialized_ || !mft_ || hw_async_mft_) return false;

    const uint32_t width = out_width_;
    const uint32_t height = out_height_;
    const uint32_t fps = out_fps_;

    const uint64_t abandoned = abandon_pending_encoder_inputs(
        pending_video_pts_, pending_input_count_, pending_inputs_abandoned_);
    if (abandoned != 0) {
        SR_LOG_ERROR(L"Encoder fallback abandoned %llu accepted input frame(s)",
                     static_cast<unsigned long long>(abandoned));
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
bool VideoEncoder::flush(std::vector<AsyncEncodedOutput>& out_samples) {
    if (!initialized_ || !mft_) return false;
    if (hw_async_mft_) return async_drain_to(out_samples, 10'000);

    bool drain_ok = true;
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

    while (true) {
        MFT_OUTPUT_DATA_BUFFER out_buf{};
        DWORD status = 0;

        if (!mft_provides_output_samples_) {
            HRESULT alloc_hr = CreateMftOutputSample(mft_output_sample_size_, mft_output_sample_alignment_, &out_buf.pSample);
            if (FAILED(alloc_hr)) {
                SR_LOG_ERROR(L"Failed to allocate MFT flush output sample: 0x%08X", alloc_hr);
                output_errors_.fetch_add(1, std::memory_order_relaxed);
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
            output_errors_.fetch_add(1, std::memory_order_relaxed);
            SR_LOG_ERROR(L"ProcessOutput during flush failed: 0x%08X", hr);
            if (out_buf.pSample) out_buf.pSample->Release();
            if (out_buf.pEvents) out_buf.pEvents->Release();
            return false;
        }
        if (out_buf.pSample) {
            if (!stamp_output_sample(out_buf.pSample, 0)) drain_ok = false;
            AsyncEncodedOutput output{};
            output.sample.Attach(out_buf.pSample);
            output.sample->GetSampleTime(&output.pts);
            out_samples.push_back(std::move(output));
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

bool VideoEncoder::async_drain_to(std::vector<AsyncEncodedOutput>& out_samples, DWORD timeout_ms) {
    const size_t reserve_count = async_scheduler_->input_count() +
        static_cast<size_t>(pending_input_count_.load(std::memory_order_relaxed)) +
        async_scheduler_->output_count() + 16;
    try {
        if (reserve_count > out_samples.max_size() - out_samples.size()) return false;
        out_samples.reserve(out_samples.size() + reserve_count);
    } catch (...) {
        SR_LOG_ERROR(L"Could not reserve output storage for asynchronous encoder drain");
        return false;
    }

    async_scheduler_->request_drain();
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool timed_out = false;
    while (GetTickCount64() < deadline) {
        AsyncEncodedOutput encoded;
        while (try_pop_async_output(encoded)) {
            out_samples.push_back(std::move(encoded));
        }

        if (async_scheduler_->failed()) break;
        if (async_encoder_drain_complete(
                async_scheduler_->worker_drained(), async_scheduler_->drain_complete(),
                async_scheduler_->output_events_pending(), async_scheduler_->input_count(),
                async_scheduler_->output_count(),
                pending_input_count_.load(std::memory_order_relaxed))) {
            break;
        }

        const ULONGLONG now_ms = GetTickCount64();
        if (now_ms >= deadline) break;
        const DWORD remaining = static_cast<DWORD>(deadline - now_ms);
        async_scheduler_->wait_for_output_or_terminal(std::chrono::milliseconds(remaining));
    }

    const bool drain_complete_before_join = async_encoder_drain_complete(
        async_scheduler_->worker_drained(), async_scheduler_->drain_complete(),
        async_scheduler_->output_events_pending(), async_scheduler_->input_count(),
        async_scheduler_->output_count(),
        pending_input_count_.load(std::memory_order_relaxed));
    if (!drain_complete_before_join && !async_scheduler_->failed()) {
        timed_out = GetTickCount64() >= deadline;
        scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
        async_scheduler_->fail(timed_out ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL);
    }
    if (async_scheduler_->failed()) {
        const uint64_t queued = static_cast<uint64_t>(async_scheduler_->abandon_queued_inputs());
        if (queued != 0) pending_inputs_abandoned_.fetch_add(queued, std::memory_order_relaxed);
    }

    // The deadline above bounds event waits, not a driver call already in
    // progress. A vendor driver stuck inside ProcessInput/ProcessOutput cannot
    // be safely force-killed here; do not detach a worker that owns this MFT.
    if (async_worker_.joinable()) async_worker_.join();
    AsyncEncodedOutput encoded;
    while (try_pop_async_output(encoded)) {
        out_samples.push_back(std::move(encoded));
    }
    if (hw_event_callback_ && !hw_event_callback_->wait_quiescent(3000)) {
        scheduler_errors_.fetch_add(1, std::memory_order_relaxed);
        async_scheduler_->fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }

    const uint64_t pending = pending_input_count_.load(std::memory_order_relaxed);
    const bool drain_ok = !async_scheduler_->failed() && async_encoder_drain_complete(
        async_scheduler_->worker_drained(), async_scheduler_->drain_complete(),
        async_scheduler_->output_events_pending(), async_scheduler_->input_count(),
        async_scheduler_->output_count(), pending) && async_pending_frames_.empty();
    if (!drain_ok && pending != 0) {
        pending_inputs_abandoned_.fetch_add(pending, std::memory_order_relaxed);
        pending_input_count_.store(0, std::memory_order_relaxed);
        async_pending_frames_.clear();
    }
    return encoder_drain_succeeded(async_scheduler_->drain_complete(), timed_out, drain_ok,
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
    async_worker_stopping_.store(true, std::memory_order_release);
    async_scheduler_->stop();
    if (hw_event_callback_) hw_event_callback_->disarm();

    // Wait until the sole owner has stopped calling IMFTransform before
    // shutting down the MFT or releasing its event generator.
    // Cooperative waits are woken below, but a driver call already in
    // progress can still make this join wait indefinitely. Process isolation
    // would be required to put a hard bound on a wedged vendor driver.
    if (async_worker_.joinable()) async_worker_.join();

    if (hw_shutdown_) {
        const HRESULT shutdown_hr = hw_shutdown_->Shutdown();
        if (FAILED(shutdown_hr) && shutdown_hr != MF_E_SHUTDOWN) {
            SR_LOG_WARN(L"Asynchronous encoder IMFShutdown::Shutdown failed: 0x%08X", shutdown_hr);
        }
    } else if (mft_) {
        const HRESULT end_hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (FAILED(end_hr)) SR_LOG_WARN(L"Encoder END_STREAMING failed during shutdown: 0x%08X", end_hr);
    }

    if (hw_event_callback_ && !hw_event_callback_->wait_quiescent(3000)) {
        SR_LOG_ERROR(L"Timed out waiting for the asynchronous MFT event callback to stop");
    }

    const uint64_t queued_inputs = static_cast<uint64_t>(async_scheduler_->abandon_queued_inputs());
    if (queued_inputs != 0) pending_inputs_abandoned_.fetch_add(queued_inputs, std::memory_order_relaxed);
    if (hw_async_mft_) {
        const uint64_t pending_inputs = pending_input_count_.exchange(0, std::memory_order_relaxed);
        if (pending_inputs != 0) pending_inputs_abandoned_.fetch_add(pending_inputs, std::memory_order_relaxed);
    } else if (!pending_video_pts_.empty()) {
        abandon_pending_encoder_inputs(pending_video_pts_, pending_input_count_, pending_inputs_abandoned_);
    }
    async_pending_frames_.clear();
    const std::size_t discarded_outputs = async_scheduler_->discard_outputs();
    if (discarded_outputs != 0) {
        output_errors_.fetch_add(discarded_outputs, std::memory_order_relaxed);
        SR_LOG_ERROR(L"Encoder shutdown discarded %zu encoded packet(s) that were not drained to the muxer",
                     discarded_outputs);
    }

    hw_event_callback_.Reset();
    hw_shutdown_.Reset();
    if (mft_) mft_.Reset();
    hw_events_.Reset();
    staging_tex_.Reset();
    staging_width_  = 0;
    staging_height_ = 0;
    initialized_ = false;
    hw_async_mft_ = false;
}

} // namespace sr
