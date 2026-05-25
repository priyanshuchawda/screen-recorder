#include <gtest/gtest.h>

#include <string>

#include "app/app_version.h"

TEST(AppVersionTest, WindowTitleMatchesReleaseVersion) {
    EXPECT_STREQ(sr::kAppDisplayVersion, L"0.3.6");

    const std::wstring title(sr::kAppWindowTitle);
    EXPECT_NE(title.find(L"v0.3.6"), std::wstring::npos);
    EXPECT_EQ(title.find(L"v1.0"), std::wstring::npos);
}
