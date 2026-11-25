#pragma once

#include <cstdint>

namespace sr {

// Keep the audio stream near video time even if a video write was slow.
// Bound each pass so a large discontinuity cannot monopolize the encoder.
inline bool continue_audio_pass(unsigned blocks_written, int64_t audio_pts,
                                int64_t video_pts, bool have_video) noexcept {
    if (!have_video) return blocks_written < 4;
    return blocks_written < 200 && audio_pts < video_pts;
}

} // namespace sr
