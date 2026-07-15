#pragma once

#include <string_view>

namespace sr::fedora {

enum class RecordingFaultKind {
    PipeWire,
    Camera,
    Audio,
    Encoder,
    Output,
    Unknown,
};

struct RecordingFault {
    RecordingFaultKind kind;
    std::string_view diagnostic_name;
    std::string_view user_message;
};

constexpr RecordingFault classify_recording_fault(std::string_view source_name) {
    if (source_name.starts_with("pipewiresrc")) {
        return {RecordingFaultKind::PipeWire, "pipewire", "Screen-share connection lost. Select Record to reconnect."};
    }
    if (source_name.starts_with("v4l2src")) {
        return {RecordingFaultKind::Camera, "camera", "Camera became unavailable. Reconnect it, then select Record."};
    }
    if (source_name.starts_with("pulsesrc")) {
        return {RecordingFaultKind::Audio, "audio", "Audio device became unavailable. Check audio settings, then select Record."};
    }
    if (source_name == "video_encoder" || source_name.ends_with("enc")) {
        return {RecordingFaultKind::Encoder, "encoder", "Video encoder failed. Select Record to retry with the available encoder."};
    }
    if (source_name.starts_with("filesink") || source_name.starts_with("mp4mux")) {
        return {RecordingFaultKind::Output, "output", "Recording output failed. Check the destination folder and free space."};
    }
    return {RecordingFaultKind::Unknown, "unknown", "Recording pipeline failed. Select Record to try again."};
}

}  // namespace sr::fedora
