#pragma once
// power_mode.h — T042: Dynamic power-mode encoder adjustment
// Reads GetSystemPowerStatus; base mode on battery clamps to 15fps/1.5Mbps/480p.
// High Quality mode keeps the requested profile unchanged on AC and battery.

#include <windows.h>
#include <algorithm>
#include "utils/render_frame.h"     // EncoderProfile
#include "utils/logging.h"

namespace sr {

class PowerModeDetector {
public:
    static constexpr uint32_t kBatteryMaxFps = 15;
    static constexpr uint32_t kBatteryMaxBitrate = 1'500'000;
    // Keep battery 480p dimensions aligned for hardware H.264 encoders.
    static constexpr uint32_t kBatteryMaxWidth = 848;
    static constexpr uint32_t kBatteryMaxHeight = 480;

    // Returns true when the system is on AC power (charger connected).
    // Returns true on failure (assumes AC to avoid unexpected quality reduction).
    static bool is_on_ac_power() {
        SYSTEM_POWER_STATUS sps{};
        if (!GetSystemPowerStatus(&sps)) return true; // assume AC on API failure
        // ACLineStatus: 0 = offline (battery), 1 = online (AC), 255 = unknown
        // Treat unknown as AC (don't penalise desktop machines without a battery).
        return sps.ACLineStatus != 0;
    }

    static EncoderProfile clamp_for_battery(const EncoderProfile& requested) {
        EncoderProfile throttled = requested;
        throttled.fps         = (std::min)(requested.fps,         kBatteryMaxFps);
        throttled.bitrate_bps = (std::min)(requested.bitrate_bps, kBatteryMaxBitrate);
        throttled.width       = (std::min)(requested.width,       kBatteryMaxWidth);
        throttled.height      = (std::min)(requested.height,      kBatteryMaxHeight);
        return throttled;
    }

    static EncoderProfile clamp_for_power_state(const EncoderProfile& requested, bool on_ac) {
        if (on_ac) {
            SR_LOG_INFO(L"[PowerMode] AC — profile: %u fps / %u bps / %ux%u",
                        requested.fps, requested.bitrate_bps,
                        requested.width, requested.height);
            return requested;
        }

        EncoderProfile throttled = clamp_for_battery(requested);
        SR_LOG_INFO(L"[PowerMode] Battery — throttling to %u fps / %u bps / %ux%u "
                    L"(requested: %u fps / %u bps / %ux%u)",
                    throttled.fps, throttled.bitrate_bps,
                    throttled.width, throttled.height,
                    requested.fps, requested.bitrate_bps,
                    requested.width, requested.height);
        return throttled;
    }

    static EncoderProfile clamp_for_quality_and_power_state(const EncoderProfile& requested,
                                                            bool on_ac,
                                                            bool high_quality) {
        if (high_quality) {
            SR_LOG_INFO(L"[PowerMode] HQ — profile: %u fps / %u bps / %ux%u "
                        L"(%s power)",
                        requested.fps, requested.bitrate_bps,
                        requested.width, requested.height,
                        on_ac ? L"AC" : L"Battery");
            return requested;
        }

        return clamp_for_power_state(requested, on_ac);
    }

    // Clamp the requested EncoderProfile for the current power state.
    //   AC power  → unchanged (use requested profile)
    //   Battery   → aggressive: 15fps / 1.5Mbps / 848x480
    static EncoderProfile clamp_for_power(const EncoderProfile& requested) {
        return clamp_for_power_state(requested, is_on_ac_power());
    }
};

} // namespace sr
