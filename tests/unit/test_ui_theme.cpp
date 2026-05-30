#include <gtest/gtest.h>

#include "app/ui_theme.h"

TEST(UiThemeTest, PrimaryButtonUsesAccentHierarchy) {
    const auto normal = sr::ui::button_visual(sr::ui::ButtonRole::Primary,
                                             sr::ui::ButtonInteraction::Normal);
    const auto hot = sr::ui::button_visual(sr::ui::ButtonRole::Primary,
                                          sr::ui::ButtonInteraction::Hot);
    const auto pressed = sr::ui::button_visual(sr::ui::ButtonRole::Primary,
                                              sr::ui::ButtonInteraction::Pressed);

    EXPECT_EQ(normal.fill, sr::ui::kAccent);
    EXPECT_EQ(hot.fill, sr::ui::kAccentHot);
    EXPECT_EQ(pressed.fill, sr::ui::kAccentPressed);
    EXPECT_EQ(normal.text, sr::ui::kTextStrong);
}

TEST(UiThemeTest, SecondaryButtonKeepsSubtleNativeContrast) {
    const auto normal = sr::ui::button_visual(sr::ui::ButtonRole::Secondary,
                                             sr::ui::ButtonInteraction::Normal);
    const auto pressed = sr::ui::button_visual(sr::ui::ButtonRole::Secondary,
                                              sr::ui::ButtonInteraction::Pressed);

    EXPECT_EQ(normal.fill, sr::ui::kSurfaceRaised);
    EXPECT_EQ(normal.border, sr::ui::kBorder);
    EXPECT_EQ(normal.text, sr::ui::kText);
    EXPECT_EQ(pressed.fill, sr::ui::kSurfacePressed);
}

TEST(UiThemeTest, DisabledButtonUsesMutedAccessibleState) {
    const auto primary_disabled =
        sr::ui::button_visual(sr::ui::ButtonRole::Primary,
                              sr::ui::ButtonInteraction::Disabled);
    const auto secondary_disabled =
        sr::ui::button_visual(sr::ui::ButtonRole::Secondary,
                              sr::ui::ButtonInteraction::Disabled);

    EXPECT_EQ(primary_disabled.fill, sr::ui::kSurfaceDisabled);
    EXPECT_EQ(secondary_disabled.fill, sr::ui::kSurfaceDisabled);
    EXPECT_EQ(primary_disabled.text, sr::ui::kTextDisabled);
    EXPECT_EQ(secondary_disabled.text, sr::ui::kTextDisabled);
}

TEST(UiThemeTest, GeometryStaysCompactAndStable) {
    EXPECT_EQ(sr::ui::kButtonCornerRadius, 6);
    EXPECT_EQ(sr::ui::kButtonFocusInset, 3);
    EXPECT_GE(sr::ui::kButtonCornerRadius, 4);
    EXPECT_LE(sr::ui::kButtonCornerRadius, 8);
}

TEST(UiThemeTest, OverlayCloseTargetIsInsetAndClickable) {
    const RECT host{0, 0, 280, 210};
    const RECT close = sr::ui::overlay_close_rect(host);

    EXPECT_EQ(close.left, 248);
    EXPECT_EQ(close.top, 8);
    EXPECT_EQ(close.right, 272);
    EXPECT_EQ(close.bottom, 32);
    EXPECT_TRUE(sr::ui::point_in_rect(close, 260, 20));
    EXPECT_FALSE(sr::ui::point_in_rect(close, 247, 20));
    EXPECT_FALSE(sr::ui::point_in_rect(close, 260, 32));
}

TEST(UiThemeTest, StatusVisualKeepsIdleQuiet) {
    const auto idle = sr::ui::status_visual(sr::ui::StatusTone::Idle, true);

    EXPECT_EQ(idle.accent, sr::ui::kStatusIdle);
    EXPECT_EQ(idle.fill, sr::ui::kSurface);
    EXPECT_EQ(idle.text, sr::ui::kTextMuted);
    EXPECT_FALSE(idle.animated);
}

TEST(UiThemeTest, StatusVisualAnimatesRecordingOnlyWhenMotionAllowed) {
    const auto motion =
        sr::ui::status_visual(sr::ui::StatusTone::Recording, true);
    const auto reduced_motion =
        sr::ui::status_visual(sr::ui::StatusTone::Recording, false);

    EXPECT_EQ(motion.accent, sr::ui::kRecording);
    EXPECT_TRUE(motion.animated);
    EXPECT_EQ(reduced_motion.accent, sr::ui::kRecording);
    EXPECT_FALSE(reduced_motion.animated);
}

TEST(UiThemeTest, StatusPulseFallsBackToStaticWhenReducedMotion) {
    EXPECT_EQ(sr::ui::status_pulse_color(sr::ui::StatusTone::Recording,
                                         false, true),
              sr::ui::kRecording);
    EXPECT_EQ(sr::ui::status_pulse_color(sr::ui::StatusTone::Recording,
                                         true, true),
              sr::ui::kRecordingHot);
    EXPECT_EQ(sr::ui::status_pulse_color(sr::ui::StatusTone::Recording,
                                         true, false),
              sr::ui::kRecording);
    EXPECT_EQ(sr::ui::status_pulse_color(sr::ui::StatusTone::Paused,
                                         true, true),
              sr::ui::kPaused);
}

TEST(UiThemeTest, StatusVisualProvidesDistinctPausedAndStoppingTone) {
    const auto paused = sr::ui::status_visual(sr::ui::StatusTone::Paused, true);
    const auto stopping =
        sr::ui::status_visual(sr::ui::StatusTone::Stopping, true);

    EXPECT_EQ(paused.accent, sr::ui::kPaused);
    EXPECT_EQ(stopping.accent, sr::ui::kStopping);
    EXPECT_FALSE(paused.animated);
    EXPECT_FALSE(stopping.animated);
    EXPECT_NE(paused.accent, stopping.accent);
}
