#include "camera_preview_policy.h"

#include <gtest/gtest.h>

TEST(CameraPreviewPolicy, AlwaysUsesAHighQualitySmoothLivePreview) {
    const auto ac = sr::fedora::camera_preview_for(sr::fedora::profile_for(false, false, true, 30));
    EXPECT_EQ(ac.width, 1280);
    EXPECT_EQ(ac.height, 720);
    EXPECT_EQ(ac.fps, 30);
    const auto battery = sr::fedora::camera_preview_for(sr::fedora::profile_for(false, false, false, 30));
    EXPECT_EQ(battery.width, 1280);
    EXPECT_EQ(battery.height, 720);
    EXPECT_EQ(battery.fps, 30);
    const auto battery_saver = sr::fedora::camera_preview_for(sr::fedora::profile_for(false, true, true, 30));
    EXPECT_EQ(battery_saver.width, 1280);
    EXPECT_EQ(battery_saver.height, 720);
    EXPECT_EQ(battery_saver.fps, 30);
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
