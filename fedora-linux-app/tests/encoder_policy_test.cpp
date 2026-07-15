#include "encoder_policy.h"

#include <gtest/gtest.h>

TEST(EncoderPolicy, PrefersHardwareAndKeepsSoftwareAsTheFinalFallback) {
    const auto candidates = sr::fedora::encoder_candidates({true, true, true, true});
    ASSERT_EQ(candidates.size(), 4U);
    EXPECT_EQ(candidates[0], sr::fedora::EncoderKind::VaLowPower);
    EXPECT_EQ(candidates[1], sr::fedora::EncoderKind::QuickSync);
    EXPECT_EQ(candidates[2], sr::fedora::EncoderKind::VaApi);
    EXPECT_EQ(candidates[3], sr::fedora::EncoderKind::OpenH264);
}

TEST(EncoderPolicy, UsesSoftwareWhenItIsTheOnlyAvailableEncoder) {
    const auto candidates = sr::fedora::encoder_candidates({false, false, false, true});
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(candidates.front(), sr::fedora::EncoderKind::OpenH264);
}
