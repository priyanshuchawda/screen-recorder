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
        uint32_t frames_captured = 0;
        uint32_t frames_encoded = 0;
        uint32_t frames_dropped = 0;
        uint32_t audio_packets = 0;
    };

    static std::wstring path_for_output(const std::wstring& output_path);
    static std::wstring format_start_summary(const StartInfo& info);
    static std::wstring format_stop_summary(const StopInfo& info);

    bool open_for_output(const std::wstring& output_path);
    void write_start(const StartInfo& info);
    void write_stop(const StopInfo& info);
    void write_failure(const std::wstring& reason);

    const std::wstring& path() const { return path_; }

private:
    void append_line(const std::wstring& line) const;

    std::wstring path_;
};

} // namespace sr
