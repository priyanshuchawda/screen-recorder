#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace sr {

enum class MediaStream : uint8_t { Video, Audio };

template <typename Payload>
struct MediaPacket {
    MediaStream stream = MediaStream::Video;
    Payload payload{};
    uint64_t bytes = 0;
    int64_t pts_100ns = 0;
    int64_t duration_100ns = 0;
    uint64_t allocated_bytes = 0;
};

enum class MediaQueueResult : uint8_t {
    Accepted,
    Closed,
    PacketLimit,
    ByteLimit,
    DurationLimit,
    InvalidPacket,
    AllocationFailure,
};

struct MediaQueueLimits {
    size_t max_packets = 1024;
    uint64_t max_bytes = 16ull * 1024 * 1024;
    int64_t max_stream_span_100ns = 5ll * 10'000'000;
    uint64_t max_allocated_bytes = 32ull * 1024 * 1024;
};

struct MediaQueueSnapshot {
    size_t packet_count = 0;
    size_t packet_high_watermark = 0;
    uint64_t queued_bytes = 0;
    uint64_t queued_bytes_high_watermark = 0;
    uint64_t queued_allocated_bytes = 0;
    uint64_t queued_allocated_bytes_high_watermark = 0;
    uint64_t total_allocated_bytes_high_watermark = 0;
    uint64_t active_payload_bytes = 0;
    uint64_t active_allocated_bytes = 0;
    uint64_t rejected_packets = 0;
    uint64_t discarded_packets = 0;
    uint64_t discarded_bytes = 0;
    uint64_t discarded_allocated_bytes = 0;
    int64_t video_span_100ns = 0;
    int64_t audio_span_100ns = 0;
    int64_t max_stream_span_100ns = 0;
    int64_t stream_span_high_watermark_100ns = 0;
    bool closed = false;
};

// Thread-safe FIFO for immutable media samples. It never evicts accepted
// packets: capacity pressure is explicit so callers can stop and preserve a
// partial recording instead of silently losing media.
template <typename Payload>
class BoundedMediaQueue {
public:
    explicit BoundedMediaQueue(const MediaQueueLimits& limits = MediaQueueLimits{})
        : limits_(limits) {}

    MediaQueueResult try_push(MediaPacket<Payload>&& packet) {
        if (packet.bytes == 0 || packet.duration_100ns < 0 || packet.pts_100ns < 0 ||
            packet.pts_100ns > (std::numeric_limits<int64_t>::max)() - packet.duration_100ns) {
            std::lock_guard lock(mutex_);
            ++rejected_packets_;
            return MediaQueueResult::InvalidPacket;
        }
        if (packet.allocated_bytes == 0) packet.allocated_bytes = packet.bytes;

        std::lock_guard lock(mutex_);
        if (closed_) {
            ++rejected_packets_;
            return MediaQueueResult::Closed;
        }
        if (queue_.size() >= limits_.max_packets) {
            ++rejected_packets_;
            return MediaQueueResult::PacketLimit;
        }
        if (packet.bytes > limits_.max_bytes || queued_bytes_ > limits_.max_bytes - packet.bytes) {
            ++rejected_packets_;
            return MediaQueueResult::ByteLimit;
        }
        const uint64_t in_flight_capacity = active_allocated_bytes_;
        if (packet.allocated_bytes > limits_.max_allocated_bytes ||
            queued_allocated_bytes_ > limits_.max_allocated_bytes - packet.allocated_bytes ||
            in_flight_capacity > limits_.max_allocated_bytes - packet.allocated_bytes -
                                     queued_allocated_bytes_) {
            ++rejected_packets_;
            return MediaQueueResult::ByteLimit;
        }
        if (span_with_packet_locked(packet.stream, packet.pts_100ns,
                                    packet.pts_100ns + packet.duration_100ns) >
            limits_.max_stream_span_100ns) {
            ++rejected_packets_;
            return MediaQueueResult::DurationLimit;
        }

        const uint64_t packet_bytes = packet.bytes;
        const uint64_t allocated_bytes = packet.allocated_bytes;
        queued_bytes_ += packet_bytes;
        queued_allocated_bytes_ += allocated_bytes;
        try {
            queue_.push_back(std::move(packet));
        } catch (...) {
            queued_bytes_ -= packet_bytes;
            queued_allocated_bytes_ -= allocated_bytes;
            ++rejected_packets_;
            return MediaQueueResult::AllocationFailure;
        }
        packet_high_watermark_ = (std::max)(packet_high_watermark_, queue_.size());
        queued_bytes_high_watermark_ = (std::max)(queued_bytes_high_watermark_, queued_bytes_);
        queued_allocated_bytes_high_watermark_ = (std::max)(
            queued_allocated_bytes_high_watermark_, queued_allocated_bytes_);
        total_allocated_bytes_high_watermark_ = (std::max)(
            total_allocated_bytes_high_watermark_, queued_allocated_bytes_ + active_allocated_bytes_);
        stream_span_high_watermark_ = (std::max)(stream_span_high_watermark_,
            span_with_packet_locked(packet.stream, packet.pts_100ns,
                                    packet.pts_100ns + packet.duration_100ns));
        cv_.notify_one();
        return MediaQueueResult::Accepted;
    }

    std::optional<MediaPacket<Payload>> try_pop() {
        std::lock_guard lock(mutex_);
        return pop_locked();
    }

    std::optional<MediaPacket<Payload>> wait_pop() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        return pop_locked(true);
    }

    // Release the byte-budget reservation after the consumer no longer owns
    // the sample's payload. This keeps an active blocking write inside the
    // same allocation budget as queued packets.
    void release_active(const MediaPacket<Payload>& packet) {
        std::lock_guard lock(mutex_);
        const uint64_t allocated = packet.allocated_bytes ? packet.allocated_bytes : packet.bytes;
        active_payload_bytes_ = packet.bytes > active_payload_bytes_
            ? 0 : active_payload_bytes_ - packet.bytes;
        active_allocated_bytes_ = allocated > active_allocated_bytes_
            ? 0 : active_allocated_bytes_ - allocated;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void close_and_discard() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            for (const auto& packet : queue_) {
                ++discarded_packets_;
                discarded_bytes_ += packet.bytes;
                discarded_allocated_bytes_ += packet.allocated_bytes
                    ? packet.allocated_bytes : packet.bytes;
            }
            queue_.clear();
            queued_bytes_ = 0;
            queued_allocated_bytes_ = 0;
        }
        cv_.notify_all();
    }

    // Reopen only between writer sessions, after the previous consumer joined.
    void reset() {
        std::lock_guard lock(mutex_);
        queue_.clear();
        queued_bytes_ = 0;
        queued_allocated_bytes_ = 0;
        active_payload_bytes_ = 0;
        active_allocated_bytes_ = 0;
        packet_high_watermark_ = 0;
        queued_bytes_high_watermark_ = 0;
        queued_allocated_bytes_high_watermark_ = 0;
        total_allocated_bytes_high_watermark_ = 0;
        rejected_packets_ = 0;
        discarded_packets_ = 0;
        discarded_bytes_ = 0;
        discarded_allocated_bytes_ = 0;
        stream_span_high_watermark_ = 0;
        closed_ = false;
    }

    MediaQueueSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        const auto video = span_locked(MediaStream::Video);
        const auto audio = span_locked(MediaStream::Audio);
        return {queue_.size(), packet_high_watermark_, queued_bytes_,
                queued_bytes_high_watermark_, queued_allocated_bytes_,
                queued_allocated_bytes_high_watermark_, total_allocated_bytes_high_watermark_,
                active_payload_bytes_,
                active_allocated_bytes_, rejected_packets_, discarded_packets_,
                discarded_bytes_, discarded_allocated_bytes_, video, audio,
                (std::max)(video, audio), stream_span_high_watermark_, closed_};
    }

private:
    static bool is_stream(const MediaPacket<Payload>& packet, MediaStream stream) {
        return packet.stream == stream;
    }

    int64_t span_locked(MediaStream stream) const {
        int64_t earliest = (std::numeric_limits<int64_t>::max)();
        int64_t latest = 0;
        for (const auto& packet : queue_) {
            if (!is_stream(packet, stream)) continue;
            earliest = (std::min)(earliest, packet.pts_100ns);
            latest = (std::max)(latest, packet.pts_100ns + packet.duration_100ns);
        }
        return earliest == (std::numeric_limits<int64_t>::max)() ? 0 : latest - earliest;
    }

    int64_t span_with_packet_locked(MediaStream stream, int64_t pts, int64_t end) const {
        int64_t earliest = pts;
        int64_t latest = end;
        for (const auto& packet : queue_) {
            if (!is_stream(packet, stream)) continue;
            earliest = (std::min)(earliest, packet.pts_100ns);
            latest = (std::max)(latest, packet.pts_100ns + packet.duration_100ns);
        }
        return latest - earliest;
    }

    std::optional<MediaPacket<Payload>> pop_locked(bool track_active = false) {
        if (queue_.empty()) return std::nullopt;
        MediaPacket<Payload> packet = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= packet.bytes;
        const uint64_t allocated = packet.allocated_bytes ? packet.allocated_bytes : packet.bytes;
        queued_allocated_bytes_ -= allocated;
        if (track_active) {
            active_payload_bytes_ += packet.bytes;
            active_allocated_bytes_ += allocated;
        }
        return packet;
    }

    const MediaQueueLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<MediaPacket<Payload>> queue_;
    uint64_t queued_bytes_ = 0;
    uint64_t queued_allocated_bytes_ = 0;
    uint64_t active_payload_bytes_ = 0;
    uint64_t active_allocated_bytes_ = 0;
    size_t packet_high_watermark_ = 0;
    uint64_t queued_bytes_high_watermark_ = 0;
    uint64_t queued_allocated_bytes_high_watermark_ = 0;
    uint64_t total_allocated_bytes_high_watermark_ = 0;
    uint64_t rejected_packets_ = 0;
    uint64_t discarded_packets_ = 0;
    uint64_t discarded_bytes_ = 0;
    uint64_t discarded_allocated_bytes_ = 0;
    int64_t stream_span_high_watermark_ = 0;
    bool closed_ = false;
};

} // namespace sr
