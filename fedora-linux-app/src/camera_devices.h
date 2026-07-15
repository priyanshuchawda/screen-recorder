#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace sr::fedora {

inline std::vector<std::string> discover_camera_device_paths(const std::filesystem::path& device_directory = "/dev") {
    std::vector<std::string> devices;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(device_directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("video") && name.size() > 5 &&
            std::all_of(name.begin() + 5, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            devices.push_back(entry.path().string());
        }
    }
    std::sort(devices.begin(), devices.end());
    return devices;
}

}  // namespace sr::fedora
