#include "profile_policy.h"

#include <gtest/gtest.h>

namespace {

using sr::fedora::profile_for;

TEST(ProfilePolicy, EfficiencyProfileUsesRequestedFpsOnAcPower) {
    const auto profile = profile_for(false, false, true, 60);
    EXPECT_EQ(profile.width, 848);
    EXPECT_EQ(profile.height, 480);
    EXPECT_EQ(profile.fps, 60);
    EXPECT_EQ(profile.bitrate_kbps, 6000);
}

TEST(ProfilePolicy, EfficiencyProfileClampsOnBattery) {
    const auto profile = profile_for(false, false, false, 60);
    EXPECT_EQ(profile.fps, 15);
    EXPECT_EQ(profile.bitrate_kbps, 1500);
}

TEST(ProfilePolicy, HighQualityRemainsExplicitOnBattery) {
    const auto profile = profile_for(true, false, false, 60);
    EXPECT_EQ(profile.width, 1920);
    EXPECT_EQ(profile.height, 1080);
    EXPECT_EQ(profile.fps, 60);
    EXPECT_EQ(profile.bitrate_kbps, 10000);
}

TEST(ProfilePolicy, InvalidFpsDefaultsToThirty) {
    const auto profile = profile_for(false, false, true, 24);
    EXPECT_EQ(profile.fps, 30);
    EXPECT_EQ(profile.bitrate_kbps, 4000);
}

TEST(ProfilePolicy, BatterySaverIsExplicitAndDoesNotOverrideHighQuality) {
    const auto saver = profile_for(false, true, true, 60);
    EXPECT_EQ(saver.width, 640);
    EXPECT_EQ(saver.height, 360);
    EXPECT_EQ(saver.fps, 15);
    EXPECT_EQ(saver.bitrate_kbps, 1000);
    EXPECT_TRUE(saver.battery_saver);

    const auto hq = profile_for(true, true, false, 60);
    EXPECT_TRUE(hq.high_quality);
    EXPECT_FALSE(hq.battery_saver);
    EXPECT_EQ(hq.width, 1920);
}

}  // namespace
