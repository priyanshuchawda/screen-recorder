#include <gtest/gtest.h>

#include "utils/resource_metrics.h"

TEST(ResourceMetricsTest, DxgiFieldsDescribeProcessUsageInBytes) {
    const auto fields = sr::format_dxgi_memory_fields(true, 17, 1'234'567, 8'765'432,
                                                     98'765'432, 123'456'789);

    EXPECT_NE(fields.find(L"gpu_memory_scope=process_dxgi"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_memory_available=1"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_memory_age_ms=17"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_local_usage_bytes=1234567"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_local_budget_bytes=8765432"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_nonlocal_usage_bytes=98765432"), std::wstring::npos);
    EXPECT_NE(fields.find(L"gpu_nonlocal_budget_bytes=123456789"), std::wstring::npos);
}

TEST(ResourceMetricsTest, UnavailableDxgiSampleIsExplicit) {
    const auto fields = sr::format_dxgi_memory_fields(false, 0, 0, 0, 0, 0);
    EXPECT_NE(fields.find(L"gpu_memory_available=0"), std::wstring::npos);
}
