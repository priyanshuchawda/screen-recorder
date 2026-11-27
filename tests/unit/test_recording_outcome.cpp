#include <gtest/gtest.h>

#include "controller/recording_outcome.h"

TEST(RecordingOutcomeTest, SuccessfulCleanFinalizationIsComplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Complete);
}

TEST(RecordingOutcomeTest, FinalizedContainerWithKnownMediaLossIsIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.audio_write_failures = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, CaptureResizeFailureMakesFinalizedRecordingIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.capture_resize_failures = 1;
    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, TextureAcquisitionFailureMakesFinalizedRecordingIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.capture_texture_acquisition_failures = 1;
    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, FinalizedContainerWithAbandonedEncoderInputsIsIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.encoder_inputs_abandoned = 2;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, DeferredEncoderInputIsNotLossUntilAbandoned) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.encoder_inputs_deferred = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Complete);
}

TEST(RecordingOutcomeTest, SuccessfullyRetriedEncoderInputsAreNotMediaLoss) {
    sr::RecordingOutcomeInputs input{};
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.encoder_inputs_deferred = 423;
    input.encoder_inputs_not_accepted = 3; // transient MFT backpressure, later accepted
    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Complete);
}

TEST(RecordingOutcomeTest, TransientEncoderBackpressureIsNotLossUntilAbandoned) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.encoder_inputs_not_accepted = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Complete);
}

TEST(RecordingOutcomeTest, MissingAudioSourceIsReportedAsIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.audio_source_start_failures = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, AudioResamplerFailureMakesFinalizedRecordingIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.audio_resampler_failures = 1;
    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, AudioMixerOverflowMakesFinalizedRecordingIncomplete) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.audio_mixer_dropped_packets = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::Incomplete);
}

TEST(RecordingOutcomeTest, FinalizeFailureTakesPrecedence) {
    sr::RecordingOutcomeInputs input;
    input.finalized = false;
    input.encoder_drain_complete = false;
    input.video_write_failures = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::FailedPartialKept);
}

TEST(RecordingOutcomeTest, NonMediaDiagnosticWarningDoesNotClaimCompleteWithoutWarnings) {
    sr::RecordingOutcomeInputs input;
    input.finalized = true;
    input.encoder_drain_complete = true;
    input.warnings = 1;

    EXPECT_EQ(sr::classify_recording_outcome(input), sr::RecordingOutcome::CompleteWithWarnings);
}
