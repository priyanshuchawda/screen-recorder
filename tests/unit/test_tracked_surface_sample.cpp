#include <gtest/gtest.h>

#include <mfapi.h>
#include <chrono>
#include <memory>
#include <thread>
#include "encoder/tracked_sample_allocator.h"

using Microsoft::WRL::ComPtr;

TEST(TrackedSurfaceSampleTest, ReturnsSurfaceLeaseAfterMediaFoundationReleasesSample) {
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));

    auto slot = std::make_shared<sr::TextureSurfaceSlot>();
    slot->available.store(false, std::memory_order_release);
    auto lease = std::make_shared<sr::TextureSurfaceLease>(slot);
    ComPtr<IMFSample> sample;
    ASSERT_TRUE(SUCCEEDED(sr::create_tracked_surface_sample(lease, &sample)));
    lease.reset();
    EXPECT_FALSE(slot->available.load(std::memory_order_acquire));

    sample.Reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!slot->available.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(slot->available.load(std::memory_order_acquire));
    EXPECT_TRUE(SUCCEEDED(MFShutdown()));
}
