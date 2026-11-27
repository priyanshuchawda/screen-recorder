#pragma once
// mux_writer.h — MP4 muxer wrapping IMFSinkWriter
// T015: Writes .partial.mp4 through a bounded worker queue and renames after successful drain.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include "storage/async_media_writer.h"
#include "utils/latency_histogram.h"

namespace sr {

using Microsoft::WRL::ComPtr;

HRESULT configure_mux_writer_attributes(IMFAttributes* attrs);

struct MuxConfig {
    // Video stream
    uint32_t video_width      = 1920;
    uint32_t video_height     = 1080;
    uint32_t video_fps_num    = 30;
    uint32_t video_fps_den    = 1;
    uint32_t video_bitrate    = 4'000'000;

    // Audio stream
    uint32_t audio_sample_rate    = 48000;
    uint16_t audio_channels       = 2;
    // 192 kbps AAC-LC gives speech and desktop audio more encoding headroom.
    uint32_t audio_bitrate        = 192'000;
    uint32_t audio_bits_per_sample= 16;   // 16 = PCM int, 32 = IEEE float
    bool     audio_is_float       = false;
};

class MuxWriter {
public:
    MuxWriter()  = default;
    ~MuxWriter() { if (initialized_) finalize(); else abort(); }

    // Create the .partial.mp4 file and configure streams.
    // partial_path: e.g. "C:\...\ScreenRec_2026-02-28.partial.mp4"
    // final_path  : e.g. "C:\...\ScreenRec_2026-02-28.mp4"
    bool initialize(const std::wstring& partial_path,
                    const std::wstring& final_path,
                    const MuxConfig& cfg);

    // Enqueue an encoded H.264 sample for the independent sink-writer thread.
    bool write_video(IMFSample* sample);

    // Enqueue a PCM audio sample for the independent sink-writer thread.
    bool write_audio(IMFSample* sample);

    // Finalize the writer; renames partial_path -> final_path on success
    bool finalize();
    // Release partially initialized writer state without renaming its partial file.
    void abort();

    bool        initialized()  const { return initialized_.load(std::memory_order_acquire); }
    bool        healthy() const { return !writer_.snapshot().failed; }
    uint64_t    bytes_written()const { return bytes_written_.load(std::memory_order_relaxed); }
    std::wstring final_path()  const { return final_path_; }
    struct StatsSnapshot {
        MediaQueueSnapshot queue;
        uint64_t samples_enqueued = 0;
        uint64_t samples_written = 0;
        uint64_t video_samples_enqueued = 0;
        uint64_t audio_samples_enqueued = 0;
        uint64_t video_samples_written = 0;
        uint64_t audio_samples_written = 0;
        uint64_t write_failures = 0;
        uint64_t invalid_sample_rejections = 0;
        uint64_t bytes_written = 0;
        uint64_t write_total_us = 0;
        uint64_t write_max_us = 0;
        uint64_t writer_drain_ms = 0;
        uint64_t sink_finalize_ms = 0;
        LatencyHistogram::Snapshot write_latency{};
        uint64_t active_sample_bytes = 0;
        uint64_t active_write_started_ms = 0;
        int64_t last_video_pts = -1;
        int64_t last_audio_pts = -1;
        HRESULT last_error = S_OK;
        bool failed = false;
    };
    StatsSnapshot snapshot() const;
    uint64_t active_write_started_ms() const {
        return active_write_started_ms_.load(std::memory_order_acquire);
    }
    int64_t last_video_pts() const { return last_video_pts_.load(std::memory_order_relaxed); }
    int64_t last_audio_pts() const { return last_audio_pts_.load(std::memory_order_relaxed); }

private:
    struct QueuedSample {
        ComPtr<IMFSample> sample;
    };

    using AsyncWriter = AsyncMediaWriter<QueuedSample>;
    bool enqueue_sample(MediaStream stream, IMFSample* sample);
    bool write_sample(AsyncWriter::Packet& packet);
    bool initialize_writer_worker();
    bool create_sink_writer_on_worker();
    void shutdown_sink_writer_on_worker();

    // Created, used, finalized, and released only on the writer worker.
    ComPtr<IMFSinkWriter> worker_sink_writer_;
    MuxConfig             cfg_{};
    std::wstring          partial_path_;
    std::wstring          final_path_;
    DWORD                 video_stream_index_ = 0;
    DWORD                 audio_stream_index_ = 1;
    std::atomic<bool>     initialized_{false};
    std::atomic<uint64_t> bytes_written_{0};
    uint64_t              audio_write_count_  = 0;
    std::atomic<uint64_t> active_write_started_ms_{0};
    std::atomic<int64_t> last_video_pts_{-1};
    std::atomic<int64_t> last_audio_pts_{-1};
    std::atomic<uint64_t> video_samples_written_{0};
    std::atomic<uint64_t> audio_samples_written_{0};
    std::atomic<uint64_t> video_samples_enqueued_{0};
    std::atomic<uint64_t> audio_samples_enqueued_{0};
    std::atomic<uint64_t> write_total_us_{0};
    std::atomic<uint64_t> write_max_us_{0};
    LatencyHistogram write_latency_;
    std::atomic<uint64_t> active_sample_bytes_{0};
    std::atomic<uint64_t> writer_drain_ms_{0};
    std::atomic<uint64_t> sink_finalize_ms_{0};
    std::atomic<uint64_t> invalid_sample_rejections_{0};
    std::atomic<HRESULT> last_error_{S_OK};
    std::atomic<HRESULT> sink_finalize_result_{E_PENDING};
    std::atomic<bool> finalize_requested_{false};
    AsyncWriter writer_{MediaQueueLimits{1024, 16ull * 1024 * 1024,
                                          5ll * 10'000'000}};
};

} // namespace sr
