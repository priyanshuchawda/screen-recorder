#pragma once

#include <cstdint>

namespace sr {

enum class RecordingOutcome {
    NotRun,
    Complete,
    CompleteWithWarnings,
    Incomplete,
    FailedPartialKept
};

struct RecordingOutcomeInputs {
    bool finalized = false;
    bool encoder_drain_complete = false;
    uint64_t frame_drops = 0;
    uint64_t capture_resize_failures = 0;
    uint64_t capture_texture_acquisition_failures = 0;
    uint64_t video_write_failures = 0;
    uint64_t audio_queue_drops = 0;
    uint64_t audio_resampler_failures = 0;
    uint64_t audio_write_failures = 0;
    uint64_t audio_source_start_failures = 0;
    uint64_t encoder_inputs_not_accepted = 0;
    uint64_t encoder_input_errors = 0;
    uint64_t encoder_inputs_deferred = 0;
    uint64_t encoder_inputs_abandoned = 0;
    uint64_t encoder_timestamp_errors = 0;
    uint64_t encoder_pending_inputs = 0;
    uint64_t warnings = 0;
};

inline RecordingOutcome classify_recording_outcome(
    const RecordingOutcomeInputs& input) noexcept {
    if (!input.finalized) return RecordingOutcome::FailedPartialKept;

    const bool media_loss = !input.encoder_drain_complete ||
        input.frame_drops != 0 ||
        input.capture_resize_failures != 0 ||
        input.capture_texture_acquisition_failures != 0 ||
        input.video_write_failures != 0 ||
        input.audio_queue_drops != 0 ||
        input.audio_resampler_failures != 0 ||
        input.audio_write_failures != 0 ||
        input.audio_source_start_failures != 0 ||
        input.encoder_input_errors != 0 ||
        input.encoder_inputs_abandoned != 0 ||
        input.encoder_timestamp_errors != 0 ||
        input.encoder_pending_inputs != 0;
    if (media_loss) return RecordingOutcome::Incomplete;
    return input.warnings != 0
        ? RecordingOutcome::CompleteWithWarnings
        : RecordingOutcome::Complete;
}

inline const wchar_t* recording_outcome_name(RecordingOutcome outcome) noexcept {
    switch (outcome) {
        case RecordingOutcome::NotRun: return L"not_run";
        case RecordingOutcome::Complete: return L"complete";
        case RecordingOutcome::CompleteWithWarnings: return L"complete_with_warnings";
        case RecordingOutcome::Incomplete: return L"incomplete";
        case RecordingOutcome::FailedPartialKept: return L"failed_partial_kept";
    }
    return L"unknown";
}

} // namespace sr
