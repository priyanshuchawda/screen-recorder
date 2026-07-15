#include "camera_preview_policy.h"

#include <gtest/gtest.h>

TEST(CameraPreviewPolicy, AppliesEfficiencySaverAndHighQualityLimits) {
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(false, false, true, 30)).fps, 10);
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(false, true, true, 30)).width, 160);
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(true, false, true, 30)).height, 720);
}

TEST(CameraPreviewPolicy, RunsOnlyWhenEnabledIdleAndCameraIsAvailable) {
    EXPECT_TRUE(sr::fedora::should_run_camera_preview(true, false, true));
    EXPECT_FALSE(sr::fedora::should_run_camera_preview(false, false, true));
    EXPECT_FALSE(sr::fedora::should_run_camera_preview(true, true, true));
    EXPECT_FALSE(sr::fedora::should_run_camera_preview(true, false, false));
}
