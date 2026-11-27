#pragma once

#include <cstdint>

namespace sr {

// A drain is complete only after the MFT's terminal condition is observed,
// all output processing succeeds, and no accepted input remains unaccounted.
inline bool encoder_drain_succeeded(bool terminal_seen,
                                    bool timed_out,
                                    bool output_processing_succeeded,
                                    uint64_t pending_inputs) noexcept {
    return terminal_seen && !timed_out && output_processing_succeeded && pending_inputs == 0;
}

} // namespace sr
