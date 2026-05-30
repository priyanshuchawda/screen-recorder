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
