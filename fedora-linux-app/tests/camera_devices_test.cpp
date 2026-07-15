#include "camera_devices.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

TEST(CameraDevices, ReturnsOnlyVideoDevicePathsInSortedOrder) {
    const auto root = std::filesystem::temp_directory_path() / "fedora-screen-recorder-camera-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "video10");
    std::ofstream(root / "video2");
    std::ofstream(root / "audio0");
    std::ofstream(root / "video-not-a-device");

    const auto devices = sr::fedora::discover_camera_device_paths(root);
    ASSERT_EQ(devices.size(), 2U);
    EXPECT_EQ(std::filesystem::path(devices[0]).filename(), "video10");
    EXPECT_EQ(std::filesystem::path(devices[1]).filename(), "video2");
    std::filesystem::remove_all(root);
}

}  // namespace
