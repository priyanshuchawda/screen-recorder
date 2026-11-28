#pragma once

#include <cstddef>
#include <cstdint>

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

inline bool async_encoder_drain_complete(bool worker_drained,
                                         bool terminal_seen,
                                         uint64_t output_events_pending,
                                         size_t input_queue_count,
                                         size_t output_queue_count,
                                         uint64_t pending_inputs) {
    return worker_drained && terminal_seen && output_events_pending == 0 &&
           input_queue_count == 0 && output_queue_count == 0 && pending_inputs == 0;
}

} // namespace sr
