// mux_writer.cpp — MP4 muxer using IMFSinkWriter
// T015: Writes to .partial.mp4, renames to .mp4 on successful finalize

#include "storage/mux_writer.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>   // eAVEncH264VProfile_Main

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sr {

bool MuxWriter::initialize(const std::wstring& partial_path,
                            const std::wstring& final_path,
                            const MuxConfig& cfg)
{
    partial_path_ = partial_path;
    final_path_   = final_path;

    // --- Sink Writer Attributes (async + throttling disabled) ---
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) { SR_LOG_ERROR(L"MFCreateAttributes failed: 0x%08X", hr); return false; }

    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING,       TRUE);

    // --- Create SinkWriter ---
    hr = MFCreateSinkWriterFromURL(partial_path.c_str(), nullptr, attrs.Get(),
                                   &sink_writer_);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"MFCreateSinkWriterFromURL('%s') failed: 0x%08X",
                     partial_path.c_str(), hr);
        return false;
    }

    // T029: Acquire exclusive write lock (FILE_SHARE_READ only).
    // MFSinkWriter already opened the file; we open a second handle from our
    // process with FILE_SHARE_READ so external processes cannot open for write.
    lock_handle_ = CreateFileW(
        partial_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,   // allow only readers, block external writers
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (lock_handle_ == INVALID_HANDLE_VALUE) {
        // Non-fatal: log and continue — recording still works, just without exclusive lock
        SR_LOG_WARN(L"Could not acquire exclusive file lock on '%s': %u",
                    partial_path.c_str(), GetLastError());
        lock_handle_ = INVALID_HANDLE_VALUE;
    } else {
        SR_LOG_INFO(L"Exclusive write lock acquired on partial file");
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
    audio_in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                         cfg.audio_channels * (cfg.audio_bits_per_sample / 8));

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

    initialized_ = true;
    SR_LOG_INFO(L"MuxWriter: writing to '%s'", partial_path.c_str());
    return true;
}

bool MuxWriter::write_video(IMFSample* sample) {
    if (!initialized_) return false;
    HRESULT hr = sink_writer_->WriteSample(video_stream_index_, sample);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"WriteSample (video) failed: 0x%08X", hr);
        return false;
    }
    // Approximate bytes: sample duration * bitrate / 8
    // (not exact, but good enough for monitoring)
    DWORD buf_len = 0;
    sample->GetTotalLength(&buf_len);
    bytes_written_ += buf_len;
    return true;
}

bool MuxWriter::write_audio(IMFSample* sample) {
    if (!initialized_) return false;
    HRESULT hr = sink_writer_->WriteSample(audio_stream_index_, sample);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"WriteSample (audio) failed: 0x%08X", hr);
        return false;
    }
    DWORD buf_len = 0;
    sample->GetTotalLength(&buf_len);
    bytes_written_ += buf_len;
    return true;
}

bool MuxWriter::finalize() {
    if (!sink_writer_) return true;
    initialized_ = false;

    HRESULT hr = sink_writer_->Finalize();
    sink_writer_.Reset();

    if (FAILED(hr)) {
        SR_LOG_ERROR(L"SinkWriter::Finalize failed: 0x%08X", hr);
        // Even on failure, try to rename so the file is accessible
    }

    // T029: Release exclusive write lock before rename so MoveFileEx succeeds
    if (lock_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(lock_handle_);
        lock_handle_ = INVALID_HANDLE_VALUE;
        SR_LOG_INFO(L"Exclusive write lock released");
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

} // namespace sr
