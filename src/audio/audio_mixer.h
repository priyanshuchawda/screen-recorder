#pragma once

#include "utils/render_frame.h"

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <optional>
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

namespace sr {

// Smoothly limit mixed audio peaks to about -1 dBFS instead of hard clipping.
inline float limit_audio_peak(float sample) noexcept {
    constexpr float start = 0.80f;
    constexpr float knee = 0.09f;
    const float magnitude = std::fabs(sample);
    if (magnitude <= start) return sample;
    const float limited = start + knee *
        (1.0f - std::exp(-(magnitude - start) / knee));
    return std::copysign((limited > 0.89f) ? 0.89f : limited, sample);
}

inline std::optional<size_t> find_loopback_mix_candidate(
    const AudioPacket& mic,
    const std::vector<AudioPacket>& loopback_packets,
    int64_t tolerance_100ns) {

    std::optional<size_t> best_index;
    int64_t best_delta = std::numeric_limits<int64_t>::max();

    for (size_t i = 0; i < loopback_packets.size(); ++i) {
        const auto& candidate = loopback_packets[i];
        if (candidate.is_silence ||
            candidate.buffer.size() != mic.buffer.size() ||
            candidate.sample_rate != mic.sample_rate ||
            candidate.channels != mic.channels) {
            continue;
        }

        const int64_t delta = std::llabs(candidate.pts - mic.pts);
        if (delta <= tolerance_100ns && delta < best_delta) {
            best_delta = delta;
            best_index = i;
        }
    }

    return best_index;
}

struct MixedAudioBlock {
    int64_t pts = 0;
    uint32_t frames = 0;
    std::vector<float> stereo;
};

// Stateful 10 ms mixer with a short reorder window. It aligns packets by PTS,
// holds output briefly so the slower WASAPI endpoint can deliver its matching
// packet, and emits monotonically timestamped float stereo blocks.
class AudioTimelineMixer {
public:
    static constexpr uint32_t kOutputRate = 48'000;
    static constexpr uint32_t kBlockFrames = 480;
    static constexpr int64_t kBlock100ns = 100'000;
    static constexpr int64_t kReorderFrames = 1'440; // 30 ms

    void push(AudioPacket packet, bool microphone) {
        if (!valid(packet)) return;
        const int64_t start = to_frame(packet.pts);
        const int64_t end = start + duration_frames(packet);
        if (end <= next_frame_) return;
        latest_end_ = (std::max)(latest_end_, end);
        if (!initialized_) {
            next_frame_ = (start / kBlockFrames) * kBlockFrames;
            initialized_ = true;
            emitted_ = false;
        } else if (!emitted_) {
            next_frame_ = (std::min)(next_frame_, (start / kBlockFrames) * kBlockFrames);
        }
        (microphone ? microphone_ : loopback_).push_back(std::move(packet));
    }

    std::optional<MixedAudioBlock> take(bool flush = false) {
        if (!initialized_) return std::nullopt;
        const int64_t block_end = next_frame_ + kBlockFrames;
        if (!flush && latest_end_ < block_end + kReorderFrames) return std::nullopt;
        if (flush && next_frame_ >= latest_end_) return std::nullopt;

        MixedAudioBlock block;
        block.pts = frames_to_pts(next_frame_);
        block.frames = kBlockFrames;
        block.stereo.assign(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
        mix_source(microphone_, block, next_frame_, block_end, true);
        mix_source(loopback_, block, next_frame_, block_end, false);
        limit(block.stereo);

        emitted_ = true;
        next_frame_ = block_end;
        discard_consumed(microphone_, next_frame_);
        discard_consumed(loopback_, next_frame_);
        if (next_frame_ >= latest_end_) {
            initialized_ = false;
            emitted_ = false;
        }
        return block;
    }

private:
    static int64_t to_frame(int64_t pts) {
        return static_cast<int64_t>(std::llround(
            static_cast<long double>((std::max)(int64_t{0}, pts)) * kOutputRate /
            10'000'000.0L));
    }
    static int64_t frames_to_pts(int64_t frame) {
        return static_cast<int64_t>(std::llround(
            static_cast<long double>(frame) * 10'000'000.0L / kOutputRate));
    }
    static bool valid(const AudioPacket& packet) {
        if (packet.sample_rate != kOutputRate || packet.channels == 0 ||
            (packet.bits_per_sample != 16 && !(packet.bits_per_sample == 32 && packet.is_float))) return false;
        const size_t bytes_per_frame = static_cast<size_t>(packet.channels) *
                                       (packet.bits_per_sample / 8);
        return packet.frame_count > 0 && packet.buffer.size() >=
            static_cast<size_t>(packet.frame_count) * bytes_per_frame;
    }
    static int64_t duration_frames(const AudioPacket& packet) {
        return static_cast<int64_t>(std::llround(
            static_cast<long double>(packet.frame_count) * kOutputRate /
            packet.sample_rate));
    }
    static float read_sample(const AudioPacket& packet, size_t frame, size_t channel) {
        channel = (std::min)(channel, static_cast<size_t>(packet.channels - 1));
        const size_t index = frame * packet.channels + channel;
        if (packet.bits_per_sample == 32 && packet.is_float) {
            float value = 0.0f;
            std::memcpy(&value, packet.buffer.data() + index * sizeof(float), sizeof(float));
            return std::clamp(value, -1.0f, 1.0f);
        }
        int16_t value = 0;
        std::memcpy(&value, packet.buffer.data() + index * sizeof(int16_t), sizeof(int16_t));
        return static_cast<float>(value) / 32768.0f;
    }
    static void mix_source(std::deque<AudioPacket>& packets, MixedAudioBlock& block,
                           int64_t block_start, int64_t block_end, bool microphone) {
        for (const auto& packet : packets) {
            const int64_t packet_start = to_frame(packet.pts);
            const int64_t packet_end = packet_start + duration_frames(packet);
            const int64_t begin = (std::max)(block_start, packet_start);
            const int64_t end = (std::min)(block_end, packet_end);
            if (end <= begin) continue;
            const float gain = microphone ? 1.0f : 0.50f; // keep speech foregrounded
            for (int64_t frame = begin; frame < end; ++frame) {
                const size_t in_frame = static_cast<size_t>(frame - packet_start);
                const size_t out = static_cast<size_t>(frame - block_start) * 2;
                block.stereo[out] += read_sample(packet, in_frame, 0) * gain;
                block.stereo[out + 1] += read_sample(packet, in_frame,
                    packet.channels > 1 ? 1 : 0) * gain;
            }
        }
    }
    static void discard_consumed(std::deque<AudioPacket>& packets, int64_t frame) {
        while (!packets.empty() && to_frame(packets.front().pts) +
               duration_frames(packets.front()) <= frame) {
            packets.pop_front();
        }
    }
    static void limit(std::vector<float>& samples) {
        constexpr float start = 0.82f;
        constexpr float knee = 0.12f;
        for (float& sample : samples) {
            const float magnitude = std::fabs(sample);
            if (magnitude > start) {
                const float compressed = start + knee *
                    (1.0f - std::exp(-(magnitude - start) / knee));
                sample = std::copysign((std::min)(compressed, 0.97f), sample);
            }
        }
    }

    std::deque<AudioPacket> microphone_;
    std::deque<AudioPacket> loopback_;
    int64_t next_frame_ = 0;
    int64_t latest_end_ = 0;
    bool initialized_ = false;
    bool emitted_ = false;
};

} // namespace sr
