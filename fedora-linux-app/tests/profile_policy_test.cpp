#include "profile_policy.h"

#include <gtest/gtest.h>

namespace {

using sr::fedora::profile_for;

TEST(ProfilePolicy, EfficiencyProfileUsesRequestedFpsOnAcPower) {
    const auto profile = profile_for(false, true, 60);
    EXPECT_EQ(profile.width, 848);
    EXPECT_EQ(profile.height, 480);
    EXPECT_EQ(profile.fps, 60);
    EXPECT_EQ(profile.bitrate_kbps, 6000);
}

TEST(ProfilePolicy, EfficiencyProfileClampsOnBattery) {
    const auto profile = profile_for(false, false, 60);
    EXPECT_EQ(profile.fps, 15);
    EXPECT_EQ(profile.bitrate_kbps, 1500);
}

TEST(ProfilePolicy, HighQualityRemainsExplicitOnBattery) {
    const auto profile = profile_for(true, false, 60);
    EXPECT_EQ(profile.width, 1920);
    EXPECT_EQ(profile.height, 1080);
    EXPECT_EQ(profile.fps, 60);
    EXPECT_EQ(profile.bitrate_kbps, 10000);
}

TEST(ProfilePolicy, InvalidFpsDefaultsToThirty) {
    const auto profile = profile_for(false, true, 24);
    EXPECT_EQ(profile.fps, 30);
    EXPECT_EQ(profile.bitrate_kbps, 4000);
}

}  // namespace
