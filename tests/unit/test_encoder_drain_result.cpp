#include <gtest/gtest.h>
#include "encoder/drain_result.h"

TEST(EncoderDrainResultTest, RequiresTerminalEventAndNoPendingInputs) {
    EXPECT_TRUE(sr::encoder_drain_succeeded(true, false, true, 0));
    EXPECT_FALSE(sr::encoder_drain_succeeded(false, false, true, 0));
    EXPECT_FALSE(sr::encoder_drain_succeeded(true, false, true, 1));
}

TEST(EncoderDrainResultTest, RejectsTimeoutAndOutputOrAllocationFailure) {
    EXPECT_FALSE(sr::encoder_drain_succeeded(false, true, true, 0));
    EXPECT_FALSE(sr::encoder_drain_succeeded(true, false, false, 0));
}
