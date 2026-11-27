#pragma once

#include <cstddef>

namespace sr {

inline constexpr std::size_t kAsyncMftInputQueueCapacity = 4;
inline constexpr std::size_t kAsyncMftMaxInFlightInputs = 4;
inline constexpr std::size_t kAsyncMftOutputQueueCapacity = 16;
inline constexpr std::size_t kAsyncMftOutputQueueByteCapacity = 8 * 1024 * 1024;

} // namespace sr
