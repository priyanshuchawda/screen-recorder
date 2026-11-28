#include "utils/session_diagnostics.h"
#include "utils/logging.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include <windows.h>

namespace sr {

std::wstring SessionDiagnostics::path_for_output(const std::wstring& output_path) {
    const std::filesystem::path output(output_path);
    const std::wstring filename = output.stem().wstring() + L".diagnostics.txt";
    return (output.parent_path() / L"_diagnostics" / filename).wstring();
}

std::wstring SessionDiagnostics::format_start_summary(const StartInfo& info) {
    wchar_t buf[2048]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"event=session_start build_commit=%s output=%s adapter=%s probed_encoder=%s "
                 L"encoder_mode=%s power=%s quality=%s profile=%ux%u@%ufps %ubps",
                 info.build_commit.empty() ? L"unknown" : info.build_commit.c_str(),
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
    wchar_t buf[2048]{};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"event=session_stop status=%s encoder_mode_end=%s "
                 L"frames_captured=%u frames_encoded=%u frames_dropped=%u "
                 L"video_scheduled_frame_drops=%llu video_unsubmitted_frames=%llu "
                 L"capture_resize_failures=%u capture_texture_acquisition_failures=%u "
                 L"queue_drops=%u video_write_failures=%u audio_queue_drops=%u "
                 L"audio_resampler_failures=%u audio_write_failures=%u "
                 L"audio_source_start_failures=%u audio_mixer_dropped_packets=%llu "
                 L"audio_mixer_rejected_packets=%llu audio_mixer_high_water_packets=%llu "
                 L"audio_mixer_high_water_bytes=%llu audio_mixer_high_water_duration_100ns=%llu "
                 L"audio_packets=%u "
                 L"encoder_drain_complete=%u encoder_pending_inputs=%llu "
                 L"encoder_deferred_inputs_pending=%llu encoder_deferred_inputs_high_watermark=%llu "
                 L"encoder_inputs_offered=%llu encoder_inputs_accepted=%llu encoder_outputs_produced=%llu "
                 L"encoder_drain_samples=%llu "
                 L"encoder_inputs_not_accepted=%llu encoder_input_backpressure_attempts=%llu "
                 L"encoder_input_backpressure_timeouts=%llu "
                 L"encoder_input_backpressure_recoveries=%llu encoder_input_backpressure_cancellations=%llu "
                 L"encoder_input_backpressure_max_wait_us=%llu encoder_input_backpressure_p95_wait_us=%llu "
                 L"encoder_input_errors=%llu "
                 L"encoder_inputs_deferred=%llu encoder_inputs_abandoned=%llu encoder_inputs_retried=%llu "
                 L"encoder_timestamp_errors=%llu encoder_input_preparation_errors=%llu "
                 L"encoder_output_errors=%llu encoder_scheduler_errors=%llu "
                 L"encoder_input_accounting_errors=%llu encoder_drain_ms=%llu finalize_ms=%llu "
                 L"diagnostic_log_dropped=%llu",
                 info.status.empty() ? L"unknown" : info.status.c_str(),
                 info.encoder_mode.empty() ? L"unknown" : info.encoder_mode.c_str(),
                 info.frames_captured,
                 info.frames_encoded,
                 info.frames_dropped,
                 static_cast<unsigned long long>(info.video_scheduled_frame_drops),
                 static_cast<unsigned long long>(info.video_unsubmitted_frames),
                 info.capture_resize_failures,
                 info.capture_texture_acquisition_failures,
                 info.queue_drops,
                 info.video_write_failures,
                 info.audio_queue_drops,
                 info.audio_resampler_failures,
                 info.audio_write_failures,
                 info.audio_source_start_failures,
                 static_cast<unsigned long long>(info.audio_mixer_dropped_packets),
                 static_cast<unsigned long long>(info.audio_mixer_rejected_packets),
                 static_cast<unsigned long long>(info.audio_mixer_high_water_packets),
                 static_cast<unsigned long long>(info.audio_mixer_high_water_bytes),
                 static_cast<unsigned long long>(info.audio_mixer_high_water_duration_100ns),
                 info.audio_packets,
                 info.encoder_drain_complete ? 1u : 0u,
                 static_cast<unsigned long long>(info.encoder_pending_inputs),
                 static_cast<unsigned long long>(info.encoder_deferred_inputs_pending),
                 static_cast<unsigned long long>(info.encoder_deferred_inputs_high_watermark),
                 static_cast<unsigned long long>(info.encoder_inputs_offered),
                 static_cast<unsigned long long>(info.encoder_inputs_accepted),
                 static_cast<unsigned long long>(info.encoder_outputs_produced),
                 static_cast<unsigned long long>(info.encoder_drain_samples),
                 static_cast<unsigned long long>(info.encoder_inputs_not_accepted),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_attempts),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_timeouts),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_recoveries),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_cancellations),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_max_wait_us),
                 static_cast<unsigned long long>(info.encoder_input_backpressure_p95_wait_us),
                 static_cast<unsigned long long>(info.encoder_input_errors),
                 static_cast<unsigned long long>(info.encoder_inputs_deferred),
                 static_cast<unsigned long long>(info.encoder_inputs_abandoned),
                 static_cast<unsigned long long>(info.encoder_inputs_retried),
                 static_cast<unsigned long long>(info.encoder_timestamp_errors),
                 static_cast<unsigned long long>(info.encoder_input_preparation_errors),
                 static_cast<unsigned long long>(info.encoder_output_errors),
                 static_cast<unsigned long long>(info.encoder_scheduler_errors),
                 static_cast<unsigned long long>(info.encoder_input_accounting_errors),
                 static_cast<unsigned long long>(info.encoder_drain_ms),
                 static_cast<unsigned long long>(info.finalize_ms),
                 static_cast<unsigned long long>(info.diagnostic_log_dropped));
    return buf;
}

bool SessionDiagnostics::open_for_output(const std::wstring& output_path) {
    path_ = path_for_output(output_path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    if (ec) {
        path_.clear();
        return false;
    }

    HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        path_.clear();
        return false;
    }
    CloseHandle(file);
    set_persistent_log_path(path_);
    append_line(L"event=diagnostics_open format=2");
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

void SessionDiagnostics::write_event(const std::wstring& event) {
    append_line(event.empty() ? L"event=unknown" : event);
}

void SessionDiagnostics::append_line(const std::wstring& line) const {
    if (path_.empty()) return;
    append_persistent_log_line(path_, line);
}

} // namespace sr
