#pragma once

#include <atomic>
#include <cstdint>
#include <deque>

namespace sr {

// Replacing an encoder invalidates any accepted frames still waiting for
// output. Keep that loss visible and make the pending counter match its PTS
// ledger instead of silently resetting one representation of the state.
inline uint64_t abandon_pending_encoder_inputs(
    std::deque<int64_t>& pending_pts,
    std::atomic<uint64_t>& pending_count,
    std::atomic<uint64_t>& abandoned_count) noexcept {
    const uint64_t abandoned = static_cast<uint64_t>(pending_pts.size());
    pending_pts.clear();
    pending_count.store(0, std::memory_order_relaxed);
    if (abandoned != 0) {
        abandoned_count.fetch_add(abandoned, std::memory_order_relaxed);
    }
    return abandoned;
}

} // namespace sr
