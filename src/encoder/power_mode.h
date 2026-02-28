#pragma once
// power_mode.h — T042: Dynamic power-mode encoder adjustment
// Reads GetSystemPowerStatus at session start; on battery, clamps to 30fps/8Mbps
// regardless of the user-configured profile, to preserve battery life.

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
    //   AC power  → requested profile returned unchanged
    //   Battery   → fps clamped to 30, bitrate clamped to 8 Mbps
    // Resolution is never altered — only fps and bitrate are throttled.
    static EncoderProfile clamp_for_power(const EncoderProfile& requested) {
        if (is_on_ac_power()) {
            SR_LOG_INFO(L"[PowerMode] AC — using requested profile: %u fps / %u bps",
                        requested.fps, requested.bitrate_bps);
            return requested;
        }

        // Battery throttle: cap at 30fps / 8 Mbps
        EncoderProfile throttled      = requested;
        throttled.fps                 = std::min(requested.fps,         30u);
        throttled.bitrate_bps         = std::min(requested.bitrate_bps, 8'000'000u);
        SR_LOG_INFO(L"[PowerMode] Battery — throttling to %u fps / %u bps "
                    L"(requested: %u fps / %u bps)",
                    throttled.fps, throttled.bitrate_bps,
                    requested.fps, requested.bitrate_bps);
        return throttled;
    }
};

} // namespace sr
