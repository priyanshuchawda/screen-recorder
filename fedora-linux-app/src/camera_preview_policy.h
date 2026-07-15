#pragma once

#include "profile_policy.h"

namespace sr::fedora {

struct CameraPreviewProfile {
    int width;
    int height;
    int fps;
};

constexpr CameraPreviewProfile camera_preview_for(const RecordingProfile& recording) {
    if (recording.high_quality) return {1280, 720, 30};
    if (recording.battery_saver) return {160, 90, 5};
    if (!recording.on_ac) return {320, 180, 10};
    return {320, 180, 10};
}

}  // namespace sr::fedora
