#include <gtest/gtest.h>

#include "app/stop_flow.h"

TEST(StopFlowTest, FirstStopRequestStartsFinalizationWithoutExit) {
    sr::StopFlow flow;

    EXPECT_TRUE(flow.begin_stop(false));
    EXPECT_TRUE(flow.stop_in_progress());
    EXPECT_FALSE(flow.exit_requested_after_stop());

    EXPECT_FALSE(flow.complete_stop());
    EXPECT_FALSE(flow.stop_in_progress());
    EXPECT_FALSE(flow.exit_requested_after_stop());
}

TEST(StopFlowTest, CloseWhileRecordingStartsFinalizationAndDefersExit) {
    sr::StopFlow flow;

    EXPECT_TRUE(flow.begin_stop(true));
    EXPECT_TRUE(flow.stop_in_progress());
    EXPECT_TRUE(flow.exit_requested_after_stop());

    EXPECT_TRUE(flow.complete_stop());
    EXPECT_FALSE(flow.stop_in_progress());
    EXPECT_FALSE(flow.exit_requested_after_stop());
}

TEST(StopFlowTest, CloseWhileStoppingDoesNotStartDuplicateFinalization) {
    sr::StopFlow flow;

    EXPECT_TRUE(flow.begin_stop(false));
    EXPECT_FALSE(flow.begin_stop(true));
    EXPECT_TRUE(flow.stop_in_progress());
    EXPECT_TRUE(flow.exit_requested_after_stop());

    EXPECT_TRUE(flow.complete_stop());
    EXPECT_FALSE(flow.stop_in_progress());
    EXPECT_FALSE(flow.exit_requested_after_stop());
}
