#pragma once

#include <cstdint>
#include <string>

namespace sr {

class SessionDiagnostics {
public:
    struct StartInfo {
        std::wstring output_path;
        std::wstring adapter_name;
        std::wstring probed_encoder_name;
        std::wstring encoder_mode;
        std::wstring power_state;
        bool high_quality = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t bitrate_bps = 0;
    };

    struct StopInfo {
        std::wstring status;
        std::wstring encoder_mode;
        uint32_t frames_captured = 0;
        uint32_t frames_encoded = 0;
        uint32_t frames_dropped = 0;
        uint32_t capture_resize_failures = 0;
        uint32_t capture_texture_acquisition_failures = 0;
        uint32_t queue_drops = 0;
        uint32_t video_write_failures = 0;
        uint32_t audio_queue_drops = 0;
        uint32_t audio_resampler_failures = 0;
        uint32_t audio_write_failures = 0;
        uint32_t audio_source_start_failures = 0;
        uint64_t audio_mixer_dropped_packets = 0;
        uint64_t audio_mixer_rejected_packets = 0;
        uint64_t audio_mixer_high_water_packets = 0;
        uint64_t audio_mixer_high_water_bytes = 0;
        uint64_t audio_mixer_high_water_duration_100ns = 0;
        uint32_t audio_packets = 0;
        bool encoder_drain_complete = true;
        uint64_t encoder_pending_inputs = 0;
        uint64_t encoder_deferred_inputs_pending = 0;
        uint64_t encoder_deferred_inputs_high_watermark = 0;
        uint64_t encoder_inputs_offered = 0;
        uint64_t encoder_inputs_accepted = 0;
        uint64_t encoder_outputs_produced = 0;
        uint64_t encoder_inputs_not_accepted = 0;
        uint64_t encoder_input_errors = 0;
        uint64_t encoder_inputs_deferred = 0;
        uint64_t encoder_inputs_abandoned = 0;
        uint64_t encoder_inputs_retried = 0;
        uint64_t encoder_timestamp_errors = 0;
        uint64_t encoder_input_preparation_errors = 0;
        uint64_t encoder_output_errors = 0;
        uint64_t encoder_scheduler_errors = 0;
        uint64_t encoder_input_accounting_errors = 0;
        uint64_t encoder_drain_samples = 0;
        uint64_t encoder_drain_ms = 0;
        uint64_t finalize_ms = 0;
        uint64_t diagnostic_log_dropped = 0;
    };

    static std::wstring path_for_output(const std::wstring& output_path);
    static std::wstring format_start_summary(const StartInfo& info);
    static std::wstring format_stop_summary(const StopInfo& info);

    bool open_for_output(const std::wstring& output_path);
    void write_start(const StartInfo& info);
    void write_stop(const StopInfo& info);
    void write_failure(const std::wstring& reason);
    void write_event(const std::wstring& event);

    const std::wstring& path() const { return path_; }

private:
    void append_line(const std::wstring& line) const;

    std::wstring path_;
};

} // namespace sr
