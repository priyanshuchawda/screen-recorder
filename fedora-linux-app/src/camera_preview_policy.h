#pragma once

#include "profile_policy.h"

namespace sr::fedora {

struct CameraPreviewProfile {
    int width;
    int height;
    int fps;
};

// The preview is useful while composing a recording, but it must never hold
// the camera while the recording pipeline needs it for a PiP track.
constexpr bool should_run_camera_preview(bool enabled, bool recording, bool camera_available) {
    return enabled && !recording && camera_available;
}

constexpr CameraPreviewProfile camera_preview_for(const RecordingProfile& recording) {
    if (recording.high_quality) return {1280, 720, 30};
    if (recording.battery_saver) return {160, 90, 5};
    if (!recording.on_ac) return {320, 180, 10};
    return {320, 180, 10};
}

}  // namespace sr::fedora
