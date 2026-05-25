#include <gtest/gtest.h>

#include "app/app_icon.h"

TEST(AppIconTest, UsesEmbeddedIconResourceId) {
    EXPECT_EQ(sr::kAppIconResourceId, 1);
    EXPECT_EQ(reinterpret_cast<ULONG_PTR>(sr::app_icon_resource_name()),
              static_cast<ULONG_PTR>(sr::kAppIconResourceId));
}
