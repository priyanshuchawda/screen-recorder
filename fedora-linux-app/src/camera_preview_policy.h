#pragma once

#include "profile_policy.h"

namespace sr::fedora {

struct CameraPreviewProfile {
    int width;
    int height;
    int fps;
};

constexpr int kCameraPreviewQueueBuffers = 1;
constexpr int kCameraPreviewDefaultWindowSize = 360;

// The preview is useful while composing a recording, but it must never hold
// the camera while the recording pipeline needs it for a PiP track.
constexpr bool should_run_camera_preview(bool enabled, bool recording, bool camera_available) {
    return enabled && !recording && camera_available;
}

constexpr CameraPreviewProfile camera_preview_for(const RecordingProfile& recording) {
    if (recording.high_quality) return {1280, 720, 30};
    if (recording.battery_saver) return {160, 90, 5};
    // Match the Windows efficiency preview on AC: smooth enough for framing
    // without turning the preview into a second high-resolution workload.
    if (recording.on_ac) return {640, 360, 20};
    // Fedora remains power-aware when unplugged, but still feels responsive.
    return {320, 180, 15};
}

}  // namespace sr::fedora
