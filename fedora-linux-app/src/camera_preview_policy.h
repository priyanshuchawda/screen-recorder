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
    // The live view is a user-facing PiP, so keep it sharp and responsive on
    // every power profile. Battery Saver continues to limit the recording,
    // encoder, and recorded camera-PiP settings independently.
    (void)recording;
    return {1280, 720, 30};
}

}  // namespace sr::fedora
