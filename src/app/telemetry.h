#pragma once
// telemetry.h — T037: Runtime telemetry counters for debug overlay
// Provides a plain snapshot struct that can be filled from session counters and
// displayed in the UI overlay without any locks (all reads are relaxed atomics).

#include <cstdint>
#include <string>
#include <atomic>

namespace sr {

// -------------------------------------------------------------------
// TelemetrySnapshot — plain-old-data copy of all live counters.
// Filled by SessionController::telemetry_snapshot() and read by UI.
// -------------------------------------------------------------------
struct TelemetrySnapshot {
    uint32_t frames_captured   = 0;  // total frames received from WGC
    uint32_t frames_encoded    = 0;  // total frames written to encoder
    uint32_t frames_dropped    = 0;  // frames lost (queue full at push)
    uint32_t frames_backlogged = 0;  // frames currently sitting in the queue
    uint32_t audio_packets     = 0;  // audio packets muxed
    uint32_t dup_frames        = 0;  // synthetic duplicates inserted by FramePacer
    uint32_t encoder_mode      = 0;  // 0 = HW, 1 = SW, 2 = SW 720p
    bool     is_on_ac          = true;

    // Human-readable encoder mode label
    const wchar_t* encoder_mode_label() const {
        switch (encoder_mode) {
            case 0:  return L"HW";
            case 1:  return L"SW";
            case 2:  return L"SW 720p";
            default: return L"?";
        }
    }
};

// -------------------------------------------------------------------
// TelemetryStore — live atomic counters owned by SessionController.
// All updates are relaxed (no ordering required — these are display-only).
// -------------------------------------------------------------------
class TelemetryStore {
public:
    // Called from capture thread
    void on_frame_captured()               { frames_captured_.fetch_add(1, std::memory_order_relaxed); }
    void on_frame_dropped()                { frames_dropped_.fetch_add(1, std::memory_order_relaxed); }

    // Called from encode thread
    void on_frame_encoded()                { frames_encoded_.fetch_add(1, std::memory_order_relaxed); }
    void on_audio_written()                { audio_written_.fetch_add(1, std::memory_order_relaxed); }
    void on_duplicate_inserted()           { dup_frames_.fetch_add(1, std::memory_order_relaxed); }

    // Called from UI thread (250ms timer) — approximate queue depth
    void set_backlog(uint32_t n)           { frames_backlogged_.store(n, std::memory_order_relaxed); }

    void reset() {
        frames_captured_.store(0,   std::memory_order_relaxed);
        frames_encoded_.store(0,    std::memory_order_relaxed);
        frames_dropped_.store(0,    std::memory_order_relaxed);
        frames_backlogged_.store(0, std::memory_order_relaxed);
        audio_written_.store(0,     std::memory_order_relaxed);
        dup_frames_.store(0,        std::memory_order_relaxed);
    }

    TelemetrySnapshot snapshot(uint32_t encoder_mode, bool on_ac) const {
        TelemetrySnapshot s;
        s.frames_captured   = frames_captured_.load(std::memory_order_relaxed);
        s.frames_encoded    = frames_encoded_.load(std::memory_order_relaxed);
        s.frames_dropped    = frames_dropped_.load(std::memory_order_relaxed);
        s.frames_backlogged = frames_backlogged_.load(std::memory_order_relaxed);
        s.audio_packets     = audio_written_.load(std::memory_order_relaxed);
        s.dup_frames        = dup_frames_.load(std::memory_order_relaxed);
        s.encoder_mode      = encoder_mode;
        s.is_on_ac          = on_ac;
        return s;
    }

private:
    std::atomic<uint32_t> frames_captured_  { 0 };
    std::atomic<uint32_t> frames_encoded_   { 0 };
    std::atomic<uint32_t> frames_dropped_   { 0 };
    std::atomic<uint32_t> frames_backlogged_{ 0 };
    std::atomic<uint32_t> audio_written_    { 0 };
    std::atomic<uint32_t> dup_frames_       { 0 };
};

} // namespace sr
