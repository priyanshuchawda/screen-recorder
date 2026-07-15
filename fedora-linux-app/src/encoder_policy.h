#pragma once

#include <vector>

namespace sr::fedora {

enum class EncoderKind {
    VaLowPower,
    QuickSync,
    VaApi,
    OpenH264,
};

struct EncoderAvailability {
    bool va_low_power{};
    bool quick_sync{};
    bool va_api{};
    bool openh264{};
};

inline std::vector<EncoderKind> encoder_candidates(const EncoderAvailability& availability) {
    std::vector<EncoderKind> candidates;
    if (availability.va_low_power) candidates.push_back(EncoderKind::VaLowPower);
    if (availability.quick_sync) candidates.push_back(EncoderKind::QuickSync);
    if (availability.va_api) candidates.push_back(EncoderKind::VaApi);
    if (availability.openh264) candidates.push_back(EncoderKind::OpenH264);
    return candidates;
}

}  // namespace sr::fedora
