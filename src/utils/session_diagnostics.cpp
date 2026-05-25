#include "utils/session_diagnostics.h"

#include <cstdio>

namespace sr {

std::wstring SessionDiagnostics::path_for_output(const std::wstring& output_path) {
    const std::wstring mp4_suffix = L".mp4";
    if (output_path.size() >= mp4_suffix.size() &&
        output_path.compare(output_path.size() - mp4_suffix.size(),
                            mp4_suffix.size(),
                            mp4_suffix) == 0) {
        return output_path.substr(0, output_path.size() - mp4_suffix.size()) +
               L".diagnostics.txt";
    }
    return output_path + L".diagnostics.txt";
}

std::wstring SessionDiagnostics::format_start_summary(const StartInfo& info) {
    wchar_t buf[2048]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"event=session_start output=%s adapter=%s probed_encoder=%s "
                 L"encoder_mode=%s power=%s quality=%s profile=%ux%u@%ufps %ubps",
                 info.output_path.c_str(),
                 info.adapter_name.empty() ? L"unknown" : info.adapter_name.c_str(),
                 info.probed_encoder_name.empty() ? L"not available" : info.probed_encoder_name.c_str(),
                 info.encoder_mode.empty() ? L"unknown" : info.encoder_mode.c_str(),
                 info.power_state.empty() ? L"unknown" : info.power_state.c_str(),
                 info.high_quality ? L"HQ" : L"Base",
                 info.width,
                 info.height,
                 info.fps,
                 info.bitrate_bps);
    return buf;
}

std::wstring SessionDiagnostics::format_stop_summary(const StopInfo& info) {
    wchar_t buf[1024]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"event=session_stop status=%s frames_captured=%u frames_encoded=%u "
                 L"frames_dropped=%u audio_packets=%u",
                 info.status.empty() ? L"unknown" : info.status.c_str(),
                 info.frames_captured,
                 info.frames_encoded,
                 info.frames_dropped,
                 info.audio_packets);
    return buf;
}

bool SessionDiagnostics::open_for_output(const std::wstring& output_path) {
    path_ = path_for_output(output_path);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path_.c_str(), L"w, ccs=UTF-8") != 0 || !file) {
        path_.clear();
        return false;
    }
    fwprintf(file, L"ScreenRecorder diagnostics\n");
    fclose(file);
    return true;
}

void SessionDiagnostics::write_start(const StartInfo& info) {
    append_line(format_start_summary(info));
}

void SessionDiagnostics::write_stop(const StopInfo& info) {
    append_line(format_stop_summary(info));
}

void SessionDiagnostics::write_failure(const std::wstring& reason) {
    append_line(L"event=session_failure reason=" + (reason.empty() ? L"unknown" : reason));
}

void SessionDiagnostics::append_line(const std::wstring& line) const {
    if (path_.empty()) return;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path_.c_str(), L"a, ccs=UTF-8") != 0 || !file) {
        return;
    }
    fwprintf(file, L"%s\n", line.c_str());
    fclose(file);
}

} // namespace sr
