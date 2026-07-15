#include "recording_faults.h"

#include <gtest/gtest.h>

TEST(RecordingFaults, ClassifiesDeviceAndOutputFailures) {
    EXPECT_EQ(sr::fedora::classify_recording_fault("pipewiresrc0").kind, sr::fedora::RecordingFaultKind::PipeWire);
    EXPECT_EQ(sr::fedora::classify_recording_fault("v4l2src0").kind, sr::fedora::RecordingFaultKind::Camera);
    EXPECT_EQ(sr::fedora::classify_recording_fault("pulsesrc0").kind, sr::fedora::RecordingFaultKind::Audio);
    EXPECT_EQ(sr::fedora::classify_recording_fault("video_encoder").kind, sr::fedora::RecordingFaultKind::Encoder);
    EXPECT_EQ(sr::fedora::classify_recording_fault("filesink0").kind, sr::fedora::RecordingFaultKind::Output);
}

TEST(RecordingFaults, GivesAUserRetryActionForUnknownFailures) {
    EXPECT_EQ(sr::fedora::classify_recording_fault("unidentified").user_message,
              "Recording pipeline failed. Select Record to try again.");
}
