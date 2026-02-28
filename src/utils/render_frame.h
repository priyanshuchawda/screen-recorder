#pragma once
// render_frame.h — Core data types for the recording pipeline
// Definitions from data-model.md

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <string>

namespace sr {

using Microsoft::WRL::ComPtr;

// Video frame from capture engine, GPU-backed
struct RenderFrame {
    ComPtr<ID3D11Texture2D> texture;
    int64_t                 pts = 0;          // QPC ticks mapped to 100ns units
    uint32_t                width = 0;
    uint32_t                height = 0;
    bool                    is_duplicate = false;

    RenderFrame() = default;
    RenderFrame(RenderFrame&&) noexcept = default;
    RenderFrame& operator=(RenderFrame&&) noexcept = default;
    RenderFrame(const RenderFrame&) = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;
};

// Audio packet from WASAPI or silence injector
struct AudioPacket {
    std::vector<uint8_t> buffer;
    uint32_t             frame_count = 0;
    int64_t              pts = 0;             // 100ns units
    bool                 is_silence = false;
    uint32_t             sample_rate = 48000;
    uint16_t             channels = 2;

    AudioPacket() = default;
    AudioPacket(AudioPacket&&) noexcept = default;
    AudioPacket& operator=(AudioPacket&&) noexcept = default;
    AudioPacket(const AudioPacket&) = default;
    AudioPacket& operator=(const AudioPacket&) = default;
};

// File context for storage manager
struct FileContext {
    std::wstring active_path;   // .partial.mp4
    std::wstring final_path;    // .mp4
    HANDLE       file_handle = INVALID_HANDLE_VALUE;
    uint64_t     bytes_written = 0;

    FileContext() = default;
    FileContext(FileContext&&) noexcept = default;
    FileContext& operator=(FileContext&&) noexcept = default;
    FileContext(const FileContext&) = delete;
    FileContext& operator=(const FileContext&) = delete;
};

// Encoder configuration profile
struct EncoderProfile {
    uint32_t fps = 30;
    uint32_t bitrate_bps = 8'000'000;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t gop_seconds = 2;
    bool     low_latency = true;
    uint32_t b_frames = 0;
    // CBR by default, Baseline/Main profile
};

} // namespace sr
