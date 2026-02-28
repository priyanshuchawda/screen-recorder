#pragma once
// mux_writer.h — MP4 muxer wrapping IMFSinkWriter
// T015: Writes .partial.mp4 with exclusive write lock. Renamed to .mp4 on Finalize().

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

namespace sr {

using Microsoft::WRL::ComPtr;

struct MuxConfig {
    // Video stream
    uint32_t video_width      = 1920;
    uint32_t video_height     = 1080;
    uint32_t video_fps_num    = 30;
    uint32_t video_fps_den    = 1;
    uint32_t video_bitrate    = 8'000'000;

    // Audio stream
    uint32_t audio_sample_rate    = 48000;
    uint16_t audio_channels       = 2;
    uint32_t audio_bitrate        = 128'000;
    uint32_t audio_bits_per_sample= 16;   // 16 = PCM int, 32 = IEEE float
    bool     audio_is_float       = false;
};

class MuxWriter {
public:
    MuxWriter()  = default;
    ~MuxWriter() { if (initialized_) finalize(); }

    // Create the .partial.mp4 file and configure streams.
    // partial_path: e.g. "C:\...\ScreenRec_2026-02-28.partial.mp4"
    // final_path  : e.g. "C:\...\ScreenRec_2026-02-28.mp4"
    bool initialize(const std::wstring& partial_path,
                    const std::wstring& final_path,
                    const MuxConfig& cfg);

    // Write an encoded H.264 sample (must be called from encode thread)
    bool write_video(IMFSample* sample);

    // Write an AAC audio sample
    bool write_audio(IMFSample* sample);

    // Finalize the writer; renames partial_path -> final_path on success
    bool finalize();

    bool        initialized()  const { return initialized_; }
    uint64_t    bytes_written()const { return bytes_written_; }
    std::wstring final_path()  const { return final_path_; }

private:
    ComPtr<IMFSinkWriter> sink_writer_;
    std::wstring          partial_path_;
    std::wstring          final_path_;
    DWORD                 video_stream_index_ = 0;
    DWORD                 audio_stream_index_ = 1;
    bool                  initialized_        = false;
    uint64_t              bytes_written_      = 0;
};

} // namespace sr
