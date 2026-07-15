#pragma once

namespace sr::fedora {

struct RecordingProfile {
    int width;
    int height;
    int fps;
    int bitrate_kbps;
    bool high_quality;
    bool battery_saver;
    bool on_ac;
};

constexpr int normalized_fps(int fps) {
    return fps == 60 ? 60 : 30;
}

// Mirrors the Windows laptop policy: base mode is aggressively clamped on
// battery, while a user-selected HQ profile remains an explicit override.
constexpr RecordingProfile profile_for(bool high_quality, bool battery_saver, bool on_ac, int requested_fps) {
    const int fps = normalized_fps(requested_fps);
    if (high_quality) {
        return {1920, 1080, fps, fps == 60 ? 10'000 : 8'000, true, false, on_ac};
    }
    if (battery_saver) {
        return {640, 360, 15, 1'000, false, true, on_ac};
    }
    if (!on_ac) {
        return {848, 480, 15, 1'500, false, false, false};
    }
    return {848, 480, fps, fps == 60 ? 6'000 : 4'000, false, false, true};
}

}  // namespace sr::fedora
