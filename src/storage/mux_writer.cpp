// mux_writer.cpp — MP4 muxer using IMFSinkWriter
// T015: Writes to .partial.mp4, renames to .mp4 on successful finalize

#include "storage/mux_writer.h"
#include "storage/probe_fault_injection.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>   // eAVEncH264VProfile_Main
#include <memory>
#include <algorithm>
#include <limits>
#include <chrono>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sr {

HRESULT configure_mux_writer_attributes(IMFAttributes* attrs) {
    if (!attrs) return E_POINTER;

    HRESULT hr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (FAILED(hr)) return hr;

    // Keep Sink Writer throttling enabled. Its default WriteSample backpressure
    // prevents real-time recordings from accumulating large internal buffers.
    return S_OK;
}

bool MuxWriter::initialize(const std::wstring& partial_path,
                            const std::wstring& final_path,
                            const MuxConfig& cfg)
{
    abort();
    partial_path_ = partial_path;
    final_path_   = final_path;
    cfg_ = cfg;

    // --- Sink Writer Attributes ---
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) { SR_LOG_ERROR(L"MFCreateAttributes failed: 0x%08X", hr); return false; }

    hr = configure_mux_writer_attributes(attrs.Get());
    if (FAILED(hr)) { SR_LOG_ERROR(L"MuxWriter attribute setup failed: 0x%08X", hr); return false; }

    // --- Create SinkWriter ---
    hr = MFCreateSinkWriterFromURL(partial_path.c_str(), nullptr, attrs.Get(),
                                   &sink_writer_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"MFCreateSinkWriterFromURL('%s') failed: 0x%08X",
                     partial_path.c_str(), hr);
        return false;
    }

    // ===================================================================
    // VIDEO STREAM — H.264 output
    // ===================================================================
    ComPtr<IMFMediaType> video_out;
    hr = MFCreateMediaType(&video_out);
    if (FAILED(hr)) return false;

    video_out->SetGUID(MF_MT_MAJOR_TYPE,     MFMediaType_Video);
    video_out->SetGUID(MF_MT_SUBTYPE,        MFVideoFormat_H264);
    video_out->SetUINT32(MF_MT_AVG_BITRATE,  cfg.video_bitrate);
    video_out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(video_out.Get(), MF_MT_FRAME_SIZE,
                       cfg.video_width, cfg.video_height);
    MFSetAttributeRatio(video_out.Get(), MF_MT_FRAME_RATE,
                        cfg.video_fps_num, cfg.video_fps_den);
    MFSetAttributeRatio(video_out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    video_out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    hr = sink_writer_->AddStream(video_out.Get(), &video_stream_index_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SinkWriter AddStream (video) failed: 0x%08X", hr);
        return false;
    }

    // Set matching input type (we pass pre-encoded H.264)
    hr = sink_writer_->SetInputMediaType(video_stream_index_, video_out.Get(), nullptr);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SetInputMediaType (video) failed: 0x%08X", hr);
        return false;
    }

    // ===================================================================
    // AUDIO STREAM — AAC-LC output
    // ===================================================================
    ComPtr<IMFMediaType> audio_out;
    hr = MFCreateMediaType(&audio_out);
    if (FAILED(hr)) return false;

    audio_out->SetGUID(MF_MT_MAJOR_TYPE,              MFMediaType_Audio);
    audio_out->SetGUID(MF_MT_SUBTYPE,                 MFAudioFormat_AAC);
    audio_out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, cfg.audio_sample_rate);
    audio_out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       cfg.audio_channels);
    audio_out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, cfg.audio_bitrate / 8);
    audio_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    16);
    audio_out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,          0); // Raw AAC

    hr = sink_writer_->AddStream(audio_out.Get(), &audio_stream_index_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SinkWriter AddStream (audio) failed: 0x%08X", hr);
        return false;
    }

    // Input audio type: PCM or IEEE Float (SinkWriter will encode to AAC)
    ComPtr<IMFMediaType> audio_in;
    hr = MFCreateMediaType(&audio_in);
    if (FAILED(hr)) return false;

    GUID audio_subtype = cfg.audio_is_float ? MFAudioFormat_Float : MFAudioFormat_PCM;
    audio_in->SetGUID(MF_MT_MAJOR_TYPE,              MFMediaType_Audio);
    audio_in->SetGUID(MF_MT_SUBTYPE,                 audio_subtype);
    audio_in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, cfg.audio_sample_rate);
    audio_in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       cfg.audio_channels);
    audio_in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    cfg.audio_bits_per_sample);
    uint32_t block_align = cfg.audio_channels * (cfg.audio_bits_per_sample / 8);
    audio_in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
    audio_in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                         cfg.audio_sample_rate * block_align);

    hr = sink_writer_->SetInputMediaType(audio_stream_index_, audio_in.Get(), nullptr);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SetInputMediaType (audio PCM) failed: 0x%08X", hr);
        return false;
    }

    // ===================================================================
    // Begin writing
    // ===================================================================
    hr = sink_writer_->BeginWriting();
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SinkWriter::BeginWriting failed: 0x%08X", hr);
        return false;
    }

    last_video_pts_.store(-1, std::memory_order_relaxed);
    last_audio_pts_.store(-1, std::memory_order_relaxed);
    bytes_written_.store(0, std::memory_order_relaxed);
    video_samples_written_.store(0, std::memory_order_relaxed);
    audio_samples_written_.store(0, std::memory_order_relaxed);
    video_samples_enqueued_.store(0, std::memory_order_relaxed);
    audio_samples_enqueued_.store(0, std::memory_order_relaxed);
    write_total_us_.store(0, std::memory_order_relaxed);
    write_max_us_.store(0, std::memory_order_relaxed);
    write_latency_.reset();
    active_sample_bytes_.store(0, std::memory_order_relaxed);
    writer_drain_ms_.store(0, std::memory_order_relaxed);
    sink_finalize_ms_.store(0, std::memory_order_relaxed);
    invalid_sample_rejections_.store(0, std::memory_order_relaxed);
    last_error_.store(S_OK, std::memory_order_relaxed);
    initialized_.store(true, std::memory_order_release);
    if (!initialize_writer_worker()) {
        initialized_.store(false, std::memory_order_release);
        SR_LOG_ERROR(L"Could not start independent media writer worker");
        return false;
    }
    SR_LOG_INFO(L"MuxWriter: writing to '%s'", partial_path.c_str());
    return true;
}

bool MuxWriter::write_video(IMFSample* sample) {
    return enqueue_sample(MediaStream::Video, sample);
}

bool MuxWriter::write_audio(IMFSample* sample) {
    return enqueue_sample(MediaStream::Audio, sample);
}

bool MuxWriter::initialize_writer_worker() {
    auto marshaled_sink_writer = std::make_shared<ComPtr<IStream>>();
    HRESULT marshal_hr = CoMarshalInterThreadInterfaceInStream(
        IID_IMFSinkWriter, sink_writer_.Get(), marshaled_sink_writer->GetAddressOf());
    if (FAILED(marshal_hr)) {
        last_error_.store(marshal_hr, std::memory_order_release);
        SR_LOG_ERROR(L"Could not marshal Sink Writer to its worker: 0x%08X", marshal_hr);
        return false;
    }

    auto com_initialized = std::make_shared<bool>(false);
    const bool started = writer_.start(
        [this](AsyncWriter::Packet& packet) { return write_sample(packet); },
        [this, com_initialized, marshaled_sink_writer] {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr)) {
                last_error_.store(hr, std::memory_order_release);
                SR_LOG_ERROR(L"Media writer worker COM initialization failed: 0x%08X", hr);
                return false;
            }
            *com_initialized = true;
            IStream* stream = marshaled_sink_writer->Detach();
            const HRESULT unmarshal_hr = CoGetInterfaceAndReleaseStream(
                stream, IID_IMFSinkWriter,
                reinterpret_cast<void**>(worker_sink_writer_.GetAddressOf()));
            if (FAILED(unmarshal_hr)) {
                last_error_.store(unmarshal_hr, std::memory_order_release);
                SR_LOG_ERROR(L"Could not unmarshal Sink Writer on worker: 0x%08X", unmarshal_hr);
                worker_sink_writer_.Reset();
                CoUninitialize();
                *com_initialized = false;
                return false;
            }
            return true;
        },
        [this, com_initialized] {
            worker_sink_writer_.Reset();
            if (*com_initialized) CoUninitialize();
        });
    if (!started && marshaled_sink_writer->Get()) {
        const HRESULT release_hr = CoReleaseMarshalData(marshaled_sink_writer->Get());
        if (FAILED(release_hr)) {
            SR_LOG_WARN(L"Could not release unused marshaled Sink Writer data: 0x%08X",
                        release_hr);
        }
    }
    if (!started && last_error_.load(std::memory_order_acquire) == S_OK) {
        last_error_.store(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY),
                          std::memory_order_release);
    }
    return started;
}

bool MuxWriter::enqueue_sample(MediaStream stream, IMFSample* sample) {
    if (!initialized_.load(std::memory_order_acquire) || !healthy() || !sample) {
        invalid_sample_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    LONGLONG pts = -1;
    DWORD bytes = 0;
    HRESULT hr = sample->GetSampleTime(&pts);
    if (FAILED(hr) || FAILED(hr = sample->GetTotalLength(&bytes)) || bytes == 0) {
        invalid_sample_rejections_.fetch_add(1, std::memory_order_relaxed);
        last_error_.store(FAILED(hr) ? hr : E_INVALIDARG, std::memory_order_release);
        SR_LOG_ERROR(L"Rejecting invalid media sample before queueing: 0x%08X", last_error_.load());
        return false;
    }

    LONGLONG duration = 0;
    hr = sample->GetSampleDuration(&duration);
    if (FAILED(hr) || duration <= 0) {
        if (stream == MediaStream::Video && cfg_.video_fps_num != 0) {
            duration = 10'000'000LL * cfg_.video_fps_den / cfg_.video_fps_num;
        } else {
            invalid_sample_rejections_.fetch_add(1, std::memory_order_relaxed);
            last_error_.store(FAILED(hr) ? hr : E_INVALIDARG, std::memory_order_release);
            SR_LOG_ERROR(L"Rejecting media sample with unavailable duration: 0x%08X", last_error_.load());
            return false;
        }
    }
    if (pts < 0 || duration <= 0 || pts > (std::numeric_limits<int64_t>::max)() - duration) {
        invalid_sample_rejections_.fetch_add(1, std::memory_order_relaxed);
        last_error_.store(E_INVALIDARG, std::memory_order_release);
        return false;
    }

    QueuedSample queued;
    queued.sample = sample;
    const auto result = writer_.enqueue(AsyncWriter::Packet{
        stream, std::move(queued), bytes, pts, duration});
    if (result == MediaQueueResult::Accepted) {
        if (stream == MediaStream::Video) {
            video_samples_enqueued_.fetch_add(1, std::memory_order_relaxed);
        } else {
            audio_samples_enqueued_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    if (result == MediaQueueResult::AllocationFailure) {
        last_error_.store(E_OUTOFMEMORY, std::memory_order_release);
    } else if (result != MediaQueueResult::Closed) {
        last_error_.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW), std::memory_order_release);
    }
    SR_LOG_ERROR(L"Media writer queue rejected %s packet (bytes=%u pts_100ns=%lld reason=%u)",
        stream == MediaStream::Video ? L"video" : L"audio", bytes, pts,
        static_cast<unsigned>(result));
    return false;
}

bool MuxWriter::write_sample(AsyncWriter::Packet& packet) {
    const ULONGLONG active_started_ms = GetTickCount64();
    const auto started = std::chrono::steady_clock::now();
    const LONGLONG pts = packet.pts_100ns;
    active_write_started_ms_.store(active_started_ms, std::memory_order_release);
    active_sample_bytes_.store(packet.bytes, std::memory_order_release);
#ifdef SR_LOCAL_RECORDING_PROBE
    if (packet.stream == MediaStream::Audio &&
        try_trigger_probe_audio_stall(++audio_write_count_)) {
        const uint32_t delay_ms = probe_audio_stall_duration_ms();
        SR_LOG_WARN(L"[ProbeFault] Injecting requested audio writer stall: %u ms", delay_ms);
        Sleep(delay_ms);
    }
#endif
    const DWORD stream_index = packet.stream == MediaStream::Video
        ? video_stream_index_ : audio_stream_index_;
    const HRESULT hr = worker_sink_writer_->WriteSample(stream_index, packet.payload.sample.Get());
    active_write_started_ms_.store(0, std::memory_order_release);
    active_sample_bytes_.store(0, std::memory_order_release);
    const uint64_t elapsed_us = static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    const uint64_t elapsed_ms = elapsed_us / 1000;
    write_total_us_.fetch_add(elapsed_us, std::memory_order_relaxed);
    write_latency_.record(elapsed_us);
    uint64_t previous = write_max_us_.load(std::memory_order_relaxed);
    while (elapsed_us > previous && !write_max_us_.compare_exchange_weak(
               previous, elapsed_us, std::memory_order_relaxed)) {}

    if (elapsed_ms >= 250) {
        SR_LOG_WARN(L"Slow %s WriteSample: elapsed_ms=%llu pts_100ns=%lld last_video_pts_100ns=%lld last_audio_pts_100ns=%lld hr=0x%08X",
            packet.stream == MediaStream::Video ? L"video" : L"audio", elapsed_ms, pts,
            last_video_pts(), last_audio_pts(), hr);
    }
    if (FAILED(hr)) {
        last_error_.store(hr, std::memory_order_release);
        SR_LOG_ERROR(L"WriteSample (%s) failed: 0x%08X",
            packet.stream == MediaStream::Video ? L"video" : L"audio", hr);
        return false;
    }

    if (packet.stream == MediaStream::Video) {
        last_video_pts_.store(pts, std::memory_order_relaxed);
        video_samples_written_.fetch_add(1, std::memory_order_relaxed);
    } else {
        last_audio_pts_.store(pts, std::memory_order_relaxed);
        audio_samples_written_.fetch_add(1, std::memory_order_relaxed);
    }
    bytes_written_.fetch_add(packet.bytes, std::memory_order_relaxed);
    return true;
}

MuxWriter::StatsSnapshot MuxWriter::snapshot() const {
    const auto state = writer_.snapshot();
    return {state.queue, state.samples_accepted, state.samples_written,
            video_samples_enqueued_.load(std::memory_order_relaxed),
            audio_samples_enqueued_.load(std::memory_order_relaxed),
            video_samples_written_.load(std::memory_order_relaxed),
            audio_samples_written_.load(std::memory_order_relaxed),
            state.write_failures,
            invalid_sample_rejections_.load(std::memory_order_relaxed),
            bytes_written_.load(std::memory_order_relaxed),
            write_total_us_.load(std::memory_order_relaxed),
            write_max_us_.load(std::memory_order_relaxed),
            writer_drain_ms_.load(std::memory_order_relaxed),
            sink_finalize_ms_.load(std::memory_order_relaxed),
            write_latency_.snapshot(),
            active_sample_bytes_.load(std::memory_order_acquire),
            active_write_started_ms_.load(std::memory_order_acquire),
            last_video_pts(), last_audio_pts(),
            last_error_.load(std::memory_order_acquire), state.failed};
}

bool MuxWriter::finalize() {
    if (!sink_writer_) return true;
    initialized_.store(false, std::memory_order_release);

    const ULONGLONG drain_started_ms = GetTickCount64();
    const bool packets_drained = writer_.finish();
    writer_drain_ms_.store(GetTickCount64() - drain_started_ms, std::memory_order_relaxed);
    const auto writer_state = snapshot();

    const ULONGLONG sink_finalize_started_ms = GetTickCount64();
    HRESULT hr = sink_writer_->Finalize();
    sink_finalize_ms_.store(GetTickCount64() - sink_finalize_started_ms, std::memory_order_relaxed);
    sink_writer_.Reset();

    if (!packets_drained || FAILED(hr)) {
        if (!packets_drained) {
            SR_LOG_ERROR(L"Media writer did not drain all accepted samples: accepted=%llu written=%llu discarded=%llu",
                writer_state.samples_enqueued, writer_state.samples_written,
                writer_state.queue.discarded_packets);
        }
        if (FAILED(hr)) SR_LOG_ERROR(L"SinkWriter::Finalize failed: 0x%08X", hr);
        SR_LOG_WARN(L"Keeping partial file (not renaming to .mp4): %s", partial_path_.c_str());
        return false;
    }

    // Rename .partial.mp4 -> .mp4
    if (!partial_path_.empty() && !final_path_.empty()) {
        if (!MoveFileExW(partial_path_.c_str(), final_path_.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            DWORD err = GetLastError();
            SR_LOG_ERROR(L"MoveFileEx '%s' -> '%s' failed: %u",
                         partial_path_.c_str(), final_path_.c_str(), err);
            return false;
        }
        SR_LOG_INFO(L"Recording saved: %s", final_path_.c_str());
    }

    return SUCCEEDED(hr);
}

void MuxWriter::abort() {
    initialized_.store(false, std::memory_order_release);
    writer_.abort();
    active_write_started_ms_.store(0, std::memory_order_release);
    active_sample_bytes_.store(0, std::memory_order_release);
    sink_writer_.Reset();
    video_stream_index_ = 0;
    audio_stream_index_ = 1;
    bytes_written_.store(0, std::memory_order_relaxed);
    audio_write_count_ = 0;
    last_video_pts_.store(-1, std::memory_order_relaxed);
    last_audio_pts_.store(-1, std::memory_order_relaxed);
    video_samples_written_.store(0, std::memory_order_relaxed);
    audio_samples_written_.store(0, std::memory_order_relaxed);
    video_samples_enqueued_.store(0, std::memory_order_relaxed);
    audio_samples_enqueued_.store(0, std::memory_order_relaxed);
    write_total_us_.store(0, std::memory_order_relaxed);
    write_max_us_.store(0, std::memory_order_relaxed);
    write_latency_.reset();
    active_sample_bytes_.store(0, std::memory_order_relaxed);
    writer_drain_ms_.store(0, std::memory_order_relaxed);
    sink_finalize_ms_.store(0, std::memory_order_relaxed);
    invalid_sample_rejections_.store(0, std::memory_order_relaxed);
    last_error_.store(S_OK, std::memory_order_relaxed);
    partial_path_.clear();
    final_path_.clear();
}

} // namespace sr
