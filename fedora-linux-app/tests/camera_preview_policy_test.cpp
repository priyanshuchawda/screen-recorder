#include "camera_preview_policy.h"

#include <gtest/gtest.h>

TEST(CameraPreviewPolicy, AppliesEfficiencySaverAndHighQualityLimits) {
    const auto ac = sr::fedora::camera_preview_for(sr::fedora::profile_for(false, false, true, 30));
    EXPECT_EQ(ac.width, 640);
    EXPECT_EQ(ac.height, 360);
    EXPECT_EQ(ac.fps, 20);
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(false, false, false, 30)).fps, 15);
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(false, true, true, 30)).width, 160);
    EXPECT_EQ(sr::fedora::camera_preview_for(sr::fedora::profile_for(true, false, true, 30)).height, 720);
    EXPECT_EQ(sr::fedora::kCameraPreviewQueueBuffers, 1);
    EXPECT_EQ(sr::fedora::kCameraPreviewDefaultWindowWidth, 304);
    EXPECT_EQ(sr::fedora::kCameraPreviewDefaultWindowHeight, 192);
}

TEST(CameraPreviewPolicy, RunsWhenEnabledAndCameraIsAvailable) {
    EXPECT_TRUE(sr::fedora::should_run_camera_preview(true, false, true));
    EXPECT_TRUE(sr::fedora::should_run_camera_preview(true, true, true));
    EXPECT_FALSE(sr::fedora::should_run_camera_preview(false, false, true));
    EXPECT_FALSE(sr::fedora::should_run_camera_preview(true, false, false));
}
