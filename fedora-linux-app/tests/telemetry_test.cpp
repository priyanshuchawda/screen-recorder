#include "telemetry.h"

#include <gtest/gtest.h>

TEST(Telemetry, FormatsAllCountersForTheUiAndDiagnostics) {
    const sr::fedora::TelemetrySnapshot snapshot{42, 18, 3};
    EXPECT_EQ(sr::fedora::format_telemetry(snapshot), "Encoded: 42  Audio: 18  QoS drops: 3");
}
