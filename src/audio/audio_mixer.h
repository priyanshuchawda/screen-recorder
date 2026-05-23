#pragma once

#include "utils/render_frame.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace sr {

inline std::optional<size_t> find_loopback_mix_candidate(
    const AudioPacket& mic,
    const std::vector<AudioPacket>& loopback_packets,
    int64_t tolerance_100ns) {

    std::optional<size_t> best_index;
    int64_t best_delta = std::numeric_limits<int64_t>::max();

    for (size_t i = 0; i < loopback_packets.size(); ++i) {
        const auto& candidate = loopback_packets[i];
        if (candidate.is_silence ||
            candidate.buffer.size() != mic.buffer.size() ||
            candidate.sample_rate != mic.sample_rate ||
            candidate.channels != mic.channels) {
            continue;
        }

        const int64_t delta = std::llabs(candidate.pts - mic.pts);
        if (delta <= tolerance_100ns && delta < best_delta) {
            best_delta = delta;
            best_index = i;
        }
    }

    return best_index;
}

} // namespace sr
