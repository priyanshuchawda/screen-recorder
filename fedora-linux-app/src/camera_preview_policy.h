#pragma once

#include "profile_policy.h"

namespace sr::fedora {

struct CameraPreviewProfile {
    int width;
    int height;
    int fps;
};

constexpr int kCameraPreviewQueueBuffers = 1;
// Compact 16:10 framing with a 16:9 camera crop, matching the approved
// movable preview shape while leaving the window freely resizable.
constexpr int kCameraPreviewDefaultWindowWidth = 304;
constexpr int kCameraPreviewDefaultWindowHeight = 192;

// A live preview remains available while recording. When the camera PiP is
// enabled, the recording pipeline tees its single V4L2 source to both the MP4
// compositor and the preview sink, so the device is never opened twice.
constexpr bool should_run_camera_preview(bool enabled, bool recording, bool camera_available) {
    (void)recording;
    return enabled && camera_available;
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
