#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace sr::fedora {

struct TelemetrySnapshot {
    std::uint64_t captured_frames{};
    std::uint64_t encoded_frames{};
    std::uint64_t audio_buffers{};
    std::uint64_t qos_drops{};
};

inline std::string format_telemetry(const TelemetrySnapshot& snapshot) {
    return std::format("Captured: {}  Encoded: {}  Audio: {}  QoS drops: {}",
                       snapshot.captured_frames, snapshot.encoded_frames,
                       snapshot.audio_buffers, snapshot.qos_drops);
}

}  // namespace sr::fedora
