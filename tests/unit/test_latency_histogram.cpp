#include <gtest/gtest.h>
#include "utils/latency_histogram.h"

TEST(LatencyHistogramTest, ReportsBoundedApproximatePercentiles) {
    sr::LatencyHistogram histogram;
    histogram.record(1);
    histogram.record(2);
    histogram.record(10);
    histogram.record(20);
    const auto snapshot = histogram.snapshot();
    EXPECT_EQ(snapshot.samples, 4u);
    EXPECT_EQ(snapshot.p50_upper_us, 2u);
    EXPECT_EQ(snapshot.p95_upper_us, 32u);
    EXPECT_EQ(snapshot.p99_upper_us, 32u);
}

TEST(LatencyHistogramTest, ResetClearsCounts) {
    sr::LatencyHistogram histogram;
    histogram.record(123);
    histogram.reset();
    EXPECT_EQ(histogram.snapshot().samples, 0u);
}
