#pragma once
// power_mode.h — T042: Dynamic power-mode encoder adjustment
// Reads GetSystemPowerStatus; on battery, aggressively clamps to 15fps/1.5Mbps/480p.
// On AC, allows up to 24fps/6Mbps for smooth recording with low resource usage.

#include <windows.h>
#include <algorithm>
#include "utils/render_frame.h"     // EncoderProfile
#include "utils/logging.h"

namespace sr {

class PowerModeDetector {
public:
    // Returns true when the system is on AC power (charger connected).
    // Returns true on failure (assumes AC to avoid unexpected quality reduction).
    static bool is_on_ac_power() {
        SYSTEM_POWER_STATUS sps{};
        if (!GetSystemPowerStatus(&sps)) return true; // assume AC on API failure
        // ACLineStatus: 0 = offline (battery), 1 = online (AC), 255 = unknown
        // Treat unknown as AC (don't penalise desktop machines without a battery).
        return sps.ACLineStatus != 0;
    }

    // Clamp the requested EncoderProfile for the current power state.
    //   AC power  → cap at 24fps / 6Mbps for stability
    //   Battery   → aggressive: 15fps / 1.5Mbps / 854x480
    static EncoderProfile clamp_for_power(const EncoderProfile& requested) {
        if (is_on_ac_power()) {
            EncoderProfile ac = requested;
            ac.fps         = (std::min)(requested.fps,         24u);
            ac.bitrate_bps = (std::min)(requested.bitrate_bps, 6'000'000u);
            SR_LOG_INFO(L"[PowerMode] AC — profile: %u fps / %u bps",
                        ac.fps, ac.bitrate_bps);
            return ac;
        }

        // Battery throttle: aggressive caps for maximum battery life
        EncoderProfile throttled = requested;
        throttled.fps         = (std::min)(requested.fps,         15u);
        throttled.bitrate_bps = (std::min)(requested.bitrate_bps, 1'500'000u);
        throttled.width       = (std::min)(requested.width,       854u);
        throttled.height      = (std::min)(requested.height,      480u);
        SR_LOG_INFO(L"[PowerMode] Battery — throttling to %u fps / %u bps / %ux%u "
                    L"(requested: %u fps / %u bps / %ux%u)",
                    throttled.fps, throttled.bitrate_bps,
                    throttled.width, throttled.height,
                    requested.fps, requested.bitrate_bps,
                    requested.width, requested.height);
        return throttled;
    }
};

} // namespace sr
