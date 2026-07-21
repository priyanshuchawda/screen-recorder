// Fedora Screen Recorder: C++20 + GTK4 + xdg-desktop-portal + PipeWire/GStreamer.
// Wayland source selection remains owned by GNOME's ScreenCast portal.

#include <adwaita.h>
#include <gst/gst.h>
#include <libportal/portal.h>
#include <libportal-gtk4/portal-gtk4.h>

#include "profile_policy.h"
#include "camera_devices.h"
#include "telemetry.h"
#include "recovery_actions.h"
#include "camera_preview_policy.h"
#include "recording_faults.h"
#include "encoder_policy.h"
#include "recording_clock.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

constexpr char kAppId[] = "io.github.screenrecorder.Fedora";

struct AppSettings {
    bool system_audio{true};
    bool microphone{};
    bool high_quality{};
    bool battery_saver{};
    bool camera{};
    bool camera_preview{true};
    std::string camera_device;
    int fps{30};
    std::string output_dir;
};

std::filesystem::path settings_path() {
    return std::filesystem::path(g_get_user_config_dir()) / "fedora-screen-recorder" / "settings.ini";
}

AppSettings load_settings() {
    AppSettings settings;
    GKeyFile* key_file = g_key_file_new();
    GError* error = nullptr;
    const auto path = settings_path().string();
    if (g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, &error)) {
        settings.system_audio = g_key_file_get_boolean(key_file, "Recording", "system_audio", nullptr);
        settings.microphone = g_key_file_get_boolean(key_file, "Recording", "microphone", nullptr);
        settings.high_quality = g_key_file_get_boolean(key_file, "Video", "high_quality", nullptr);
        settings.battery_saver = g_key_file_get_boolean(key_file, "Video", "battery_saver", nullptr);
        settings.camera = g_key_file_get_boolean(key_file, "Camera", "enabled", nullptr);
        GError* preview_error = nullptr;
        const bool camera_preview = g_key_file_get_boolean(key_file, "Camera", "preview", &preview_error);
        if (!preview_error) settings.camera_preview = camera_preview;
        g_clear_error(&preview_error);
        gchar* camera_device = g_key_file_get_string(key_file, "Camera", "device", nullptr);
        if (camera_device) settings.camera_device = camera_device;
        g_free(camera_device);
        settings.fps = g_key_file_get_integer(key_file, "Video", "fps", nullptr);
        gchar* output = g_key_file_get_string(key_file, "Storage", "output_dir", nullptr);
        if (output) settings.output_dir = output;
        g_free(output);
    }
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return settings;
}

void save_settings(const AppSettings& settings) {
    std::error_code error;
    std::filesystem::create_directories(settings_path().parent_path(), error);
    if (error) return;
    GKeyFile* key_file = g_key_file_new();
    g_key_file_set_boolean(key_file, "Recording", "system_audio", settings.system_audio);
    g_key_file_set_boolean(key_file, "Recording", "microphone", settings.microphone);
    g_key_file_set_boolean(key_file, "Video", "high_quality", settings.high_quality);
    g_key_file_set_boolean(key_file, "Video", "battery_saver", settings.battery_saver);
    g_key_file_set_boolean(key_file, "Camera", "enabled", settings.camera);
    g_key_file_set_boolean(key_file, "Camera", "preview", settings.camera_preview);
    g_key_file_set_string(key_file, "Camera", "device", settings.camera_device.c_str());
    g_key_file_set_integer(key_file, "Video", "fps", settings.fps);
    g_key_file_set_string(key_file, "Storage", "output_dir", settings.output_dir.c_str());
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, nullptr);
    const auto path = settings_path().string();
    g_file_set_contents(path.c_str(), data, static_cast<gssize>(length), nullptr);
    g_free(data);
    g_key_file_unref(key_file);
}

std::string default_output_directory() {
    const char* videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    return (std::filesystem::path(videos ? videos : g_get_home_dir()) / "Screen Recordings").string();
}

std::string output_path(const std::string& configured_output_dir) {
    const auto directory = std::filesystem::path(configured_output_dir.empty() ? default_output_directory() : configured_output_dir);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {};
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::format("{:%Y-%m-%d_%H-%M-%S}", now);
    auto candidate = directory / std::format("Screen Recording {}.partial.mp4", stamp);
    for (unsigned index = 2; std::filesystem::exists(candidate); ++index) {
        candidate = directory / std::format("Screen Recording {} ({}).partial.mp4", stamp, index);
    }
    return candidate.string();
}

std::string final_path_for(const std::string& partial) {
    constexpr std::string_view suffix = ".partial.mp4";
    if (partial.ends_with(suffix)) {
        return partial.substr(0, partial.size() - suffix.size()) + ".mp4";
    }
    return partial + ".mp4";
}

bool has_element(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

using sr::fedora::RecordingProfile;

struct EncoderChoice {
    std::string element;
    std::string name;
    bool hardware;
};

bool is_on_ac_power() {
    // UPower is present on Fedora Workstation. Treat lookup failures as AC so a
    // desktop machine never receives an unexpected quality reduction.
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!bus) {
        g_clear_error(&error);
        return true;
    }
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.UPower", "/org/freedesktop/UPower",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.UPower", "OnBattery"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    g_object_unref(bus);
    if (!reply) {
        g_clear_error(&error);
        return true;
    }
    GVariant* boxed = nullptr;
    g_variant_get(reply, "(@v)", &boxed);
    GVariant* value = g_variant_get_variant(boxed);
    const bool on_battery = g_variant_get_boolean(value);
    g_variant_unref(value);
    g_variant_unref(boxed);
    g_variant_unref(reply);
    return !on_battery;
}

RecordingProfile selected_profile(bool high_quality, bool battery_saver, int fps) {
    return sr::fedora::profile_for(high_quality, battery_saver, is_on_ac_power(), fps);
}

std::vector<EncoderChoice> choose_encoders(const RecordingProfile& profile) {
    // The Iris Xe driver exposes VA low-power H.264. These branches preserve
    // DMABUF/VAMemory through vapostproc, avoiding a CPU encoder on the normal
    // no-camera path. OpenH264 is a portable fallback only.
    std::vector<EncoderChoice> candidates;
    const auto available = sr::fedora::encoder_candidates({
        has_element("vah264lpenc"), has_element("qsvh264enc"), has_element("vah264enc"), has_element("openh264enc")});
    for (const auto kind : available) {
        switch (kind) {
            case sr::fedora::EncoderKind::VaLowPower:
                candidates.push_back({std::format("vah264lpenc bitrate={} key-int-max={}", profile.bitrate_kbps, profile.fps * 2), "Intel VA-API low-power H.264", true});
                break;
            case sr::fedora::EncoderKind::QuickSync:
                candidates.push_back({std::format("qsvh264enc bitrate={} gop-size={}", profile.bitrate_kbps, profile.fps * 2), "Intel Quick Sync H.264", true});
                break;
            case sr::fedora::EncoderKind::VaApi:
                candidates.push_back({std::format("vah264enc bitrate={} key-int-max={}", profile.bitrate_kbps, profile.fps * 2), "VA-API H.264", true});
                break;
            case sr::fedora::EncoderKind::OpenH264:
                candidates.push_back({std::format("openh264enc bitrate={} gop-size={} complexity=low", profile.bitrate_kbps * 1000, profile.fps * 2), "OpenH264 software fallback", false});
                break;
        }
    }
    return candidates;
}

std::string named_encoder_element(const EncoderChoice& encoder) {
    const auto separator = encoder.element.find(' ');
    if (separator == std::string::npos) return encoder.element + " name=video_encoder";
    return encoder.element.substr(0, separator) + " name=video_encoder" + encoder.element.substr(separator);
}

class RecorderWindow {
public:
    explicit RecorderWindow(AdwApplication* application)
        : portal_(xdp_portal_new()), settings_(load_settings()) {
        // AdwApplicationWindow intentionally owns its title bar. A GTK
        // application window is required here because this app supplies a
        // draggable header with explicit window controls.
        window_ = GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));
        gtk_window_set_title(window_, "Fedora Screen Recorder");
        gtk_window_set_default_size(window_, 560, 620);
        g_signal_connect(window_, "close-request", G_CALLBACK(on_close), this);
        build_ui();
    }

    ~RecorderWindow() {
        stop_camera_preview();
        cleanup_pipeline();
        if (session_) {
            xdp_session_close(session_);
            g_object_unref(session_);
        }
        g_clear_object(&portal_);
    }

    void present() {
        gtk_window_present(window_);
        open_configured_camera_preview();
        if (g_strcmp0(g_getenv("SCREEN_RECORDER_UI_SMOKE"), "preferences") == 0) {
            adw_dialog_present(ADW_DIALOG(settings_dialog_), GTK_WIDGET(window_));
        }
    }

private:
    GtkWindow* window_{};
    XdpPortal* portal_{};
    XdpSession* session_{};
    GstElement* pipeline_{};
    GstElement* camera_preview_pipeline_{};
    GtkWindow* camera_preview_window_{};
    guint bus_watch_{};
    guint camera_preview_bus_watch_{};
    guint timer_{};
    guint disk_check_{};
    guint power_check_{};
    int remote_fd_{-1};
    std::string partial_path_;
    sr::fedora::RecordingClock recording_clock_;
    bool recording_{};
    bool stopping_{};
    bool muted_{};
    // True when the visible preview is a branch of pipeline_ rather than its
    // own V4L2 pipeline. Closing that window must not stop the recording.
    bool camera_preview_from_recording_{};
    AppSettings settings_;
    RecordingProfile active_profile_{848, 480, 30, 4000, false, false, true};
    std::optional<EncoderChoice> active_encoder_;
    std::size_t active_candidate_index_{};
    std::size_t active_candidate_count_{};
    bool active_audio_{};
    bool active_camera_{};
    std::atomic_uint64_t captured_frames_{};
    std::atomic_uint64_t encoded_frames_{};
    std::atomic_uint64_t audio_buffers_{};
    std::atomic_uint64_t qos_drops_{};

    GtkButton* record_button_{};
    GtkButton* pause_button_{};
    GtkButton* stop_button_{};
    GtkButton* mute_button_{};
    GtkLabel* status_label_{};
    GtkLabel* time_label_{};
    GtkSwitch* audio_switch_{};
    GtkSwitch* microphone_switch_{};
    GtkSwitch* high_quality_switch_{};
    GtkSwitch* battery_saver_switch_{};
    GtkSwitch* camera_switch_{};
    GtkSwitch* camera_preview_switch_{};
    GtkDropDown* camera_device_dropdown_{};
    GtkDropDown* fps_dropdown_{};
    GtkLabel* profile_label_{};
    GtkLabel* output_label_{};
    GtkLabel* telemetry_label_{};
    GtkButton* recovery_button_{};
    GtkButton* preferences_button_{};
    GtkButton* preview_camera_button_{};
    GtkButton* choose_folder_button_{};
    GtkButton* maximize_window_button_{};
    GtkButton* camera_preview_maximize_button_{};
    AdwPreferencesDialog* settings_dialog_{};
    AdwActionRow* settings_output_row_{};
    std::vector<std::filesystem::path> orphaned_recordings_;
    std::vector<std::string> camera_devices_;

    static void append(GtkBox* box, GtkWidget* child) { gtk_box_append(box, child); }

    static AdwActionRow* make_action_row(const char* title, const char* subtitle) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(row, subtitle);
        adw_action_row_set_subtitle_lines(row, 2);
        return row;
    }

    static AdwPreferencesGroup* make_preferences_group(const char* title, const char* description = nullptr) {
        auto* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(group, title);
        if (description) adw_preferences_group_set_description(group, description);
        return group;
    }

    GtkSwitch* add_switch_row(AdwPreferencesGroup* group, const char* title, const char* subtitle, bool active) {
        auto* row = make_action_row(title, subtitle);
        auto* toggle = GTK_SWITCH(gtk_switch_new());
        gtk_switch_set_active(toggle, active);
        gtk_widget_set_valign(GTK_WIDGET(toggle), GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(row, GTK_WIDGET(toggle));
        adw_action_row_set_activatable_widget(row, GTK_WIDGET(toggle));
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return toggle;
    }

    void build_ui() {
        auto* header = GTK_HEADER_BAR(gtk_header_bar_new());
        gtk_header_bar_set_show_title_buttons(header, FALSE);
        auto* header_title = adw_window_title_new("Fedora Screen Recorder", "Power-aware PipeWire capture");
        gtk_header_bar_set_title_widget(header, header_title);

        auto* minimize = gtk_button_new_from_icon_name("window-minimize-symbolic");
        gtk_widget_set_tooltip_text(minimize, "Minimize");
        g_signal_connect(minimize, "clicked", G_CALLBACK(on_minimize_window), this);
        gtk_header_bar_pack_end(header, minimize);

        maximize_window_button_ = GTK_BUTTON(gtk_button_new_from_icon_name("window-maximize-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(maximize_window_button_), "Maximize");
        g_signal_connect(maximize_window_button_, "clicked", G_CALLBACK(on_toggle_maximize_window), this);
        gtk_header_bar_pack_end(header, GTK_WIDGET(maximize_window_button_));

        auto* close = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_set_tooltip_text(close, "Close");
        g_signal_connect(close, "clicked", G_CALLBACK(on_close_window_button), this);
        gtk_header_bar_pack_end(header, close);
        gtk_window_set_titlebar(window_, GTK_WIDGET(header));
        g_signal_connect(window_, "notify::maximized", G_CALLBACK(on_window_maximized_changed), this);

        auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_margin_start(content, 24);
        gtk_widget_set_margin_end(content, 24);
        gtk_widget_set_margin_top(content, 24);
        gtk_widget_set_margin_bottom(content, 24);

        auto* title = gtk_label_new("Screen Recorder");
        gtk_widget_add_css_class(title, "title-1");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        append(GTK_BOX(content), title);

        auto* detail = gtk_label_new("A compact dashboard for secure Wayland recording.");
        gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
        gtk_label_set_xalign(GTK_LABEL(detail), 0.0F);
        gtk_widget_add_css_class(detail, "dim-label");
        append(GTK_BOX(content), detail);

        auto* duration_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* duration = gtk_label_new("Duration");
        gtk_widget_set_hexpand(duration, TRUE);
        gtk_label_set_xalign(GTK_LABEL(duration), 0.0F);
        time_label_ = GTK_LABEL(gtk_label_new("00:00:00"));
        gtk_widget_add_css_class(GTK_WIDGET(time_label_), "title-3");
        append(GTK_BOX(duration_row), duration);
        append(GTK_BOX(duration_row), GTK_WIDGET(time_label_));
        append(GTK_BOX(content), duration_row);

        auto* output_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        auto* output_title = gtk_label_new("Recording folder");
        gtk_label_set_xalign(GTK_LABEL(output_title), 0.0F);
        output_label_ = GTK_LABEL(gtk_label_new(output_directory().c_str()));
        gtk_label_set_xalign(output_label_, 0.0F);
        gtk_label_set_ellipsize(output_label_, PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_add_css_class(GTK_WIDGET(output_label_), "dim-label");
        append(GTK_BOX(output_box), output_title);
        append(GTK_BOX(output_box), GTK_WIDGET(output_label_));
        append(GTK_BOX(content), output_box);

        profile_label_ = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(profile_label_, 0.0F);
        gtk_widget_add_css_class(GTK_WIDGET(profile_label_), "dim-label");
        append(GTK_BOX(content), GTK_WIDGET(profile_label_));

        telemetry_label_ = GTK_LABEL(gtk_label_new("Captured: 0  Encoded: 0  Audio: 0  QoS drops: 0"));
        gtk_label_set_xalign(telemetry_label_, 0.0F);
        gtk_widget_add_css_class(GTK_WIDGET(telemetry_label_), "dim-label");
        append(GTK_BOX(content), GTK_WIDGET(telemetry_label_));

        status_label_ = GTK_LABEL(gtk_label_new("Ready — secure portal capture, MP4 finalization, and diagnostics enabled"));
        gtk_label_set_xalign(status_label_, 0.0F);
        gtk_label_set_wrap(status_label_, TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(status_label_), "dim-label");
        append(GTK_BOX(content), GTK_WIDGET(status_label_));

        recovery_button_ = GTK_BUTTON(gtk_button_new_with_label("Recover unfinished recordings"));
        gtk_widget_set_halign(GTK_WIDGET(recovery_button_), GTK_ALIGN_START);
        g_signal_connect(recovery_button_, "clicked", G_CALLBACK(on_recover_orphans), this);
        append(GTK_BOX(content), GTK_WIDGET(recovery_button_));

        auto* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(controls, GTK_ALIGN_CENTER);
        record_button_ = GTK_BUTTON(gtk_button_new_with_label("Record"));
        gtk_widget_add_css_class(GTK_WIDGET(record_button_), "suggested-action");
        pause_button_ = GTK_BUTTON(gtk_button_new_with_label("Pause"));
        mute_button_ = GTK_BUTTON(gtk_button_new_with_label("Mute"));
        stop_button_ = GTK_BUTTON(gtk_button_new_with_label("Stop"));
        gtk_widget_add_css_class(GTK_WIDGET(stop_button_), "destructive-action");
        gtk_widget_set_sensitive(GTK_WIDGET(pause_button_), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(mute_button_), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(stop_button_), FALSE);
        g_signal_connect(record_button_, "clicked", G_CALLBACK(on_record), this);
        g_signal_connect(pause_button_, "clicked", G_CALLBACK(on_pause), this);
        g_signal_connect(mute_button_, "clicked", G_CALLBACK(on_mute), this);
        g_signal_connect(stop_button_, "clicked", G_CALLBACK(on_stop), this);
        append(GTK_BOX(controls), GTK_WIDGET(record_button_));
        append(GTK_BOX(controls), GTK_WIDGET(pause_button_));
        append(GTK_BOX(controls), GTK_WIDGET(mute_button_));
        append(GTK_BOX(controls), GTK_WIDGET(stop_button_));
        append(GTK_BOX(content), controls);

        preferences_button_ = GTK_BUTTON(gtk_button_new_with_label("Recording settings"));
        gtk_widget_set_halign(GTK_WIDGET(preferences_button_), GTK_ALIGN_CENTER);
        g_signal_connect(preferences_button_, "clicked", G_CALLBACK(on_open_preferences), this);
        append(GTK_BOX(content), GTK_WIDGET(preferences_button_));
        gtk_window_set_child(window_, content);

        build_preferences_dialog();
        refresh_profile_label();
        report_orphaned_recordings();
    }

    void build_preferences_dialog() {
        settings_dialog_ = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
        adw_dialog_set_title(ADW_DIALOG(settings_dialog_), "Recording settings");
        adw_dialog_set_content_width(ADW_DIALOG(settings_dialog_), 620);
        adw_dialog_set_content_height(ADW_DIALOG(settings_dialog_), 620);
        adw_preferences_dialog_set_search_enabled(settings_dialog_, FALSE);

        auto* recording_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_title(recording_page, "Recording");
        adw_preferences_page_set_icon_name(recording_page, "media-record-symbolic");
        auto* audio_group = make_preferences_group("Audio", "Choose which audio tracks are written to the MP4.");
        audio_switch_ = add_switch_row(audio_group, "System audio", "Include current desktop output.", settings_.system_audio);
        microphone_switch_ = add_switch_row(audio_group, "Microphone", "Optional voice track with a light noise gate.", settings_.microphone);
        adw_preferences_page_add(recording_page, audio_group);
        adw_preferences_dialog_add(settings_dialog_, recording_page);

        auto* video_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_title(video_page, "Video & Power");
        adw_preferences_page_set_icon_name(video_page, "video-display-symbolic");
        auto* profile_group = make_preferences_group("Recording profile", "Hardware-first profiles preserve battery life by default.");
        high_quality_switch_ = add_switch_row(profile_group, "High quality mode", "1080p profile; remains opt-in on battery.", settings_.high_quality);
        battery_saver_switch_ = add_switch_row(profile_group, "Battery Saver", "640×360, 15 FPS, 1 Mbps. High Quality takes precedence.", settings_.battery_saver);
        auto* fps_row = make_action_row("Frame rate", "30 FPS is recommended for battery life; 60 FPS prioritizes motion.");
        const char* fps_options[] = {"30 FPS", "60 FPS", nullptr};
        fps_dropdown_ = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(fps_options));
        gtk_drop_down_set_selected(fps_dropdown_, settings_.fps == 60 ? 1 : 0);
        adw_action_row_add_suffix(fps_row, GTK_WIDGET(fps_dropdown_));
        adw_action_row_set_activatable_widget(fps_row, GTK_WIDGET(fps_dropdown_));
        adw_preferences_group_add(profile_group, GTK_WIDGET(fps_row));
        adw_preferences_page_add(video_page, profile_group);
        adw_preferences_dialog_add(settings_dialog_, video_page);

        auto* camera_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_title(camera_page, "Camera");
        adw_preferences_page_set_icon_name(camera_page, "camera-video-symbolic");
        auto* camera_group = make_preferences_group("Camera", "Preview and recorded picture-in-picture are controlled independently.");
        camera_switch_ = add_switch_row(camera_group, "Camera overlay", "Low-rate picture-in-picture recorded into the MP4.", settings_.camera);
        camera_devices_ = sr::fedora::discover_camera_device_paths();
        auto* camera_device_row = make_action_row("Camera device", "Choose the V4L2 camera used for preview and picture-in-picture.");
        auto* camera_device_model = gtk_string_list_new(nullptr);
        for (const auto& device : camera_devices_) gtk_string_list_append(camera_device_model, device.c_str());
        camera_device_dropdown_ = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(camera_device_model), nullptr));
        g_object_unref(camera_device_model);
        if (!camera_devices_.empty()) {
            const auto saved = std::find(camera_devices_.begin(), camera_devices_.end(), settings_.camera_device);
            gtk_drop_down_set_selected(camera_device_dropdown_, static_cast<guint>(saved == camera_devices_.end() ? 0 : saved - camera_devices_.begin()));
        }
        adw_action_row_add_suffix(camera_device_row, GTK_WIDGET(camera_device_dropdown_));
        adw_action_row_set_activatable_widget(camera_device_row, GTK_WIDGET(camera_device_dropdown_));
        adw_preferences_group_add(camera_group, GTK_WIDGET(camera_device_row));
        camera_preview_switch_ = add_switch_row(camera_group, "Live camera preview", "Opens by default and stays visible while recording.", settings_.camera_preview);
        auto* preview_row = make_action_row("Open live preview", "Show the selected camera now.");
        preview_camera_button_ = GTK_BUTTON(gtk_button_new_with_label("Open"));
        adw_action_row_add_suffix(preview_row, GTK_WIDGET(preview_camera_button_));
        adw_action_row_set_activatable_widget(preview_row, GTK_WIDGET(preview_camera_button_));
        adw_preferences_group_add(camera_group, GTK_WIDGET(preview_row));
        if (camera_devices_.empty()) {
            gtk_widget_set_sensitive(GTK_WIDGET(camera_switch_), FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(camera_device_dropdown_), FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(camera_preview_switch_), FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(preview_camera_button_), FALSE);
        }
        adw_preferences_page_add(camera_page, camera_group);
        adw_preferences_dialog_add(settings_dialog_, camera_page);

        auto* storage_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_title(storage_page, "Storage");
        adw_preferences_page_set_icon_name(storage_page, "folder-symbolic");
        auto* storage_group = make_preferences_group("Recording folder", "Completed recordings and diagnostics are stored here.");
        settings_output_row_ = make_action_row("Location", output_directory().c_str());
        adw_action_row_set_subtitle_selectable(settings_output_row_, TRUE);
        choose_folder_button_ = GTK_BUTTON(gtk_button_new_with_label("Choose…"));
        adw_action_row_add_suffix(settings_output_row_, GTK_WIDGET(choose_folder_button_));
        adw_action_row_set_activatable_widget(settings_output_row_, GTK_WIDGET(choose_folder_button_));
        adw_preferences_group_add(storage_group, GTK_WIDGET(settings_output_row_));
        adw_preferences_page_add(storage_page, storage_group);
        adw_preferences_dialog_add(settings_dialog_, storage_page);

        g_signal_connect(audio_switch_, "notify::active", G_CALLBACK(on_settings_changed), this);
        g_signal_connect(microphone_switch_, "notify::active", G_CALLBACK(on_settings_changed), this);
        g_signal_connect(high_quality_switch_, "notify::active", G_CALLBACK(on_settings_changed), this);
        g_signal_connect(battery_saver_switch_, "notify::active", G_CALLBACK(on_settings_changed), this);
        g_signal_connect(camera_switch_, "notify::active", G_CALLBACK(on_settings_changed), this);
        g_signal_connect(camera_preview_switch_, "notify::active", G_CALLBACK(on_camera_preview_changed), this);
        g_signal_connect(camera_device_dropdown_, "notify::selected", G_CALLBACK(on_camera_device_changed), this);
        g_signal_connect(fps_dropdown_, "notify::selected", G_CALLBACK(on_fps_changed), this);
        g_signal_connect(preview_camera_button_, "clicked", G_CALLBACK(on_preview_camera), this);
        g_signal_connect(choose_folder_button_, "clicked", G_CALLBACK(on_choose_folder), this);
    }

    void set_status(const std::string& text) { gtk_label_set_text(status_label_, text.c_str()); }

    void report_orphaned_recordings() {
        std::error_code error;
        orphaned_recordings_.clear();
        for (const auto& entry : std::filesystem::directory_iterator(output_directory(), error)) {
            if (entry.is_regular_file() && entry.path().filename().string().ends_with(".partial.mp4")) {
                orphaned_recordings_.push_back(entry.path());
            }
        }
        gtk_widget_set_visible(GTK_WIDGET(recovery_button_), !orphaned_recordings_.empty());
        if (!orphaned_recordings_.empty()) {
            const auto count = orphaned_recordings_.size();
            set_status(std::format("Found {} unfinished recording{}; choose Recover to rename safely.", count, count == 1 ? "" : "s"));
        }
    }

    static void on_recover_orphans(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        unsigned recovered = 0;
        unsigned skipped = 0;
        for (const auto& partial : self->orphaned_recordings_) {
            if (sr::fedora::recover_partial_recording(partial) == sr::fedora::RecoveryResult::Recovered) ++recovered;
            else ++skipped;
        }
        self->report_orphaned_recordings();
        self->set_status(std::format("Recovered {} unfinished recording{}; {} skipped to avoid data loss.",
            recovered, recovered == 1 ? "" : "s", skipped));
    }

    void set_controls(bool is_recording) {
        gtk_widget_set_sensitive(GTK_WIDGET(record_button_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(pause_button_), is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(stop_button_), is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(preferences_button_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(mute_button_), is_recording &&
            (gtk_switch_get_active(audio_switch_) || gtk_switch_get_active(microphone_switch_)));
        gtk_widget_set_sensitive(GTK_WIDGET(audio_switch_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(microphone_switch_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(high_quality_switch_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(battery_saver_switch_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(camera_switch_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(camera_device_dropdown_), !is_recording && !camera_devices_.empty());
        gtk_widget_set_sensitive(GTK_WIDGET(camera_preview_switch_), !is_recording && !camera_devices_.empty());
        gtk_widget_set_sensitive(GTK_WIDGET(preview_camera_button_), !is_recording && !camera_devices_.empty());
        gtk_widget_set_sensitive(GTK_WIDGET(choose_folder_button_), !is_recording);
        gtk_widget_set_sensitive(GTK_WIDGET(recovery_button_), !is_recording && !orphaned_recordings_.empty());
        gtk_widget_set_sensitive(GTK_WIDGET(fps_dropdown_), !is_recording);
        if (!is_recording) open_configured_camera_preview();
    }

    std::string output_directory() const {
        return settings_.output_dir.empty() ? default_output_directory() : settings_.output_dir;
    }

    std::string selected_camera_device() const {
        const auto index = gtk_drop_down_get_selected(camera_device_dropdown_);
        return index < camera_devices_.size() ? camera_devices_[index] : "";
    }

    RecordingProfile profile() const {
        return selected_profile(gtk_switch_get_active(high_quality_switch_),
                                gtk_switch_get_active(battery_saver_switch_), settings_.fps);
    }

    void refresh_profile_label() {
        const auto active = profile();
        gtk_label_set_text(profile_label_, std::format(
            "{} · {}×{} · {} FPS · {} Mbps · {}",
            active.high_quality ? "High quality" : active.battery_saver ? "Battery Saver" : "Efficiency",
            active.width, active.height, active.fps, active.bitrate_kbps / 1000,
            active.on_ac ? "AC power" : "Battery saver").c_str());
    }

    void sync_settings() {
        settings_.system_audio = gtk_switch_get_active(audio_switch_);
        settings_.microphone = gtk_switch_get_active(microphone_switch_);
        settings_.high_quality = gtk_switch_get_active(high_quality_switch_);
        settings_.battery_saver = gtk_switch_get_active(battery_saver_switch_);
        settings_.camera = gtk_switch_get_active(camera_switch_);
        settings_.camera_preview = gtk_switch_get_active(camera_preview_switch_);
        settings_.camera_device = selected_camera_device();
        settings_.fps = gtk_drop_down_get_selected(fps_dropdown_) == 1 ? 60 : 30;
        save_settings(settings_);
    }

    static void on_settings_changed(GtkSwitch*, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        self->sync_settings();
        self->refresh_profile_label();
    }

    static void on_fps_changed(GtkDropDown*, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        self->sync_settings();
        self->refresh_profile_label();
    }

    static void on_camera_device_changed(GtkDropDown*, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        self->sync_settings();
        self->stop_camera_preview();
        self->open_configured_camera_preview();
    }

    static void on_camera_preview_changed(GtkSwitch*, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        self->sync_settings();
        if (gtk_switch_get_active(self->camera_preview_switch_)) self->open_configured_camera_preview();
        else self->stop_camera_preview();
    }

    static void on_preview_camera(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        gtk_switch_set_active(self->camera_preview_switch_, TRUE);
        self->open_configured_camera_preview();
    }

    static void on_open_preferences(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->recording_) adw_dialog_present(ADW_DIALOG(self->settings_dialog_), GTK_WIDGET(self->window_));
    }

    void open_configured_camera_preview() {
        if (!sr::fedora::should_run_camera_preview(settings_.camera_preview, recording_, !camera_devices_.empty())) return;
        if (!camera_preview_pipeline_) start_camera_preview();
    }

    void start_camera_preview() {
        stop_camera_preview();
        const auto device = selected_camera_device();
        if (device.empty() || !std::filesystem::exists(device)) {
            set_status("Select an available V4L2 camera before opening preview.");
            return;
        }
        if (!has_element("gtk4paintablesink")) {
            set_status("GStreamer GTK4 video sink is unavailable; install gstreamer1-plugin-gtk4.");
            return;
        }
        const auto preview = sr::fedora::camera_preview_for(profile());
        gchar* escaped_device = g_strescape(device.c_str(), nullptr);
        const auto description = std::format(
            "v4l2src device=\"{}\" do-timestamp=true ! queue max-size-buffers={} max-size-bytes=0 max-size-time=0 leaky=downstream "
            "! videoconvert ! videoscale ! videorate drop-only=true ! video/x-raw,format=BGRA,width={},height={},framerate={}/1 "
            "! gtk4paintablesink name=camera_preview_sink sync=false", escaped_device, sr::fedora::kCameraPreviewQueueBuffers,
            preview.width, preview.height, preview.fps);
        g_free(escaped_device);
        GError* error = nullptr;
        camera_preview_pipeline_ = gst_parse_launch(description.c_str(), &error);
        if (!camera_preview_pipeline_) {
            set_status(std::format("Could not start camera preview: {}", error ? error->message : "unknown error"));
            g_clear_error(&error);
            return;
        }
        GstBus* preview_bus = gst_element_get_bus(camera_preview_pipeline_);
        camera_preview_bus_watch_ = gst_bus_add_watch(preview_bus, on_camera_preview_bus_message, this);
        gst_object_unref(preview_bus);
        GstElement* sink = gst_bin_get_by_name(GST_BIN(camera_preview_pipeline_), "camera_preview_sink");
        const bool preview_opened = sink && open_camera_preview_window(sink, false);
        if (sink) gst_object_unref(sink);
        if (!preview_opened) {
            set_status("GStreamer could not create the GTK4 camera preview.");
            stop_camera_preview();
            return;
        }
        if (gst_element_set_state(camera_preview_pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            set_status("Camera preview could not acquire the selected device.");
            stop_camera_preview();
            return;
        }
        set_status(std::format("Camera preview: {}×{} at {} FPS", preview.width, preview.height, preview.fps));
    }

    bool open_camera_preview_window(GstElement* sink, bool from_recording) {
        GdkPaintable* paintable = nullptr;
        g_object_get(sink, "paintable", &paintable, nullptr);
        if (!paintable) return false;

        GtkWidget* widget = gtk_picture_new_for_paintable(paintable);
        g_object_unref(paintable);
        gtk_picture_set_can_shrink(GTK_PICTURE(widget), TRUE);
        // A floating preview should stay useful at any user-selected size.
        // Cover avoids the large letterbox bands produced by contain fitting.
        gtk_picture_set_content_fit(GTK_PICTURE(widget), GTK_CONTENT_FIT_COVER);
        camera_preview_window_ = GTK_WINDOW(gtk_window_new());
        camera_preview_from_recording_ = from_recording;
        gtk_window_set_title(camera_preview_window_, "Camera Preview");
        gtk_window_set_default_size(camera_preview_window_, sr::fedora::kCameraPreviewDefaultWindowWidth,
                                    sr::fedora::kCameraPreviewDefaultWindowHeight);
        gtk_window_set_resizable(camera_preview_window_, TRUE);
        gtk_window_set_modal(camera_preview_window_, FALSE);
        install_camera_preview_chrome();
        gtk_window_set_child(camera_preview_window_, widget);
        g_signal_connect(camera_preview_window_, "close-request", G_CALLBACK(on_preview_close), this);
        gtk_window_present(camera_preview_window_);
        return true;
    }

    void install_camera_preview_chrome() {
        auto* header = GTK_HEADER_BAR(gtk_header_bar_new());
        gtk_header_bar_set_show_title_buttons(header, FALSE);
        auto* title = adw_window_title_new("Camera Preview", "Movable and resizable live view");
        gtk_header_bar_set_title_widget(header, title);

        auto* minimize = gtk_button_new_from_icon_name("window-minimize-symbolic");
        gtk_widget_set_tooltip_text(minimize, "Minimize preview");
        g_signal_connect(minimize, "clicked", G_CALLBACK(on_minimize_preview_window), this);
        gtk_header_bar_pack_end(header, minimize);

        camera_preview_maximize_button_ = GTK_BUTTON(gtk_button_new_from_icon_name("window-maximize-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(camera_preview_maximize_button_), "Maximize preview");
        g_signal_connect(camera_preview_maximize_button_, "clicked", G_CALLBACK(on_toggle_maximize_preview_window), this);
        gtk_header_bar_pack_end(header, GTK_WIDGET(camera_preview_maximize_button_));

        auto* close = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_set_tooltip_text(close, "Close preview");
        g_signal_connect(close, "clicked", G_CALLBACK(on_close_preview_window), this);
        gtk_header_bar_pack_end(header, close);
        gtk_window_set_titlebar(camera_preview_window_, GTK_WIDGET(header));
        g_signal_connect(camera_preview_window_, "notify::maximized", G_CALLBACK(on_preview_window_maximized_changed), this);
    }

    static gboolean on_preview_close(GtkWindow* preview_window, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (self->camera_preview_window_ != preview_window) return FALSE;
        self->camera_preview_window_ = nullptr;
        const bool from_recording = self->camera_preview_from_recording_;
        self->camera_preview_from_recording_ = false;
        if (from_recording) {
            // The preview is only one tee branch of the recording pipeline.
            // Let the window close without interrupting the MP4 capture.
            gtk_switch_set_active(self->camera_preview_switch_, FALSE);
            self->set_status("Live camera preview closed; recording continues.");
            return FALSE;
        }
        self->stop_camera_preview();
        gtk_switch_set_active(self->camera_preview_switch_, FALSE);
        self->set_status("Live camera preview closed.");
        return FALSE;
    }

    static gboolean on_camera_preview_bus_message(GstBus*, GstMessage* message, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR && GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS) {
            return G_SOURCE_CONTINUE;
        }
        self->camera_preview_bus_watch_ = 0;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            self->stop_camera_preview();
            gtk_switch_set_active(self->camera_preview_switch_, FALSE);
            self->set_status("Camera preview ended. Turn on Live camera preview to reopen it.");
            return G_SOURCE_REMOVE;
        }
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        const auto* source = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
        const auto fault = sr::fedora::classify_recording_fault(source ? source : "unknown");
        const auto detail = std::string{error ? error->message : "unknown error"};
        g_clear_error(&error);
        g_free(debug);
        self->stop_camera_preview();
        gtk_switch_set_active(self->camera_preview_switch_, FALSE);
        self->set_status(std::format("Camera preview stopped: {} ({})", fault.user_message, detail));
        return G_SOURCE_REMOVE;
    }

    static void on_minimize_window(GtkButton*, gpointer data) {
        gtk_window_minimize(static_cast<RecorderWindow*>(data)->window_);
    }

    static void on_minimize_preview_window(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (self->camera_preview_window_) gtk_window_minimize(self->camera_preview_window_);
    }

    static void on_toggle_maximize_window(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (gtk_window_is_maximized(self->window_)) gtk_window_unmaximize(self->window_);
        else gtk_window_maximize(self->window_);
    }

    static void on_toggle_maximize_preview_window(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->camera_preview_window_) return;
        if (gtk_window_is_maximized(self->camera_preview_window_)) gtk_window_unmaximize(self->camera_preview_window_);
        else gtk_window_maximize(self->camera_preview_window_);
    }

    static void on_close_window_button(GtkButton*, gpointer data) {
        gtk_window_close(static_cast<RecorderWindow*>(data)->window_);
    }

    static void on_close_preview_window(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (self->camera_preview_window_) gtk_window_close(self->camera_preview_window_);
    }

    static void on_window_maximized_changed(GtkWindow* window, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        const bool maximized = gtk_window_is_maximized(window);
        gtk_button_set_icon_name(self->maximize_window_button_, maximized ? "window-restore-symbolic" : "window-maximize-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(self->maximize_window_button_), maximized ? "Restore" : "Maximize");
    }

    static void on_preview_window_maximized_changed(GtkWindow* window, GParamSpec*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        const bool maximized = gtk_window_is_maximized(window);
        gtk_button_set_icon_name(self->camera_preview_maximize_button_, maximized ? "window-restore-symbolic" : "window-maximize-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(self->camera_preview_maximize_button_), maximized ? "Restore preview" : "Maximize preview");
    }

    void stop_camera_preview() {
        if (camera_preview_bus_watch_) {
            g_source_remove(camera_preview_bus_watch_);
            camera_preview_bus_watch_ = 0;
        }
        if (camera_preview_pipeline_) {
            gst_element_set_state(camera_preview_pipeline_, GST_STATE_NULL);
            gst_object_unref(camera_preview_pipeline_);
            camera_preview_pipeline_ = nullptr;
        }
        if (camera_preview_window_) {
            auto* preview_window = camera_preview_window_;
            camera_preview_window_ = nullptr;
            gtk_window_destroy(preview_window);
        }
        camera_preview_from_recording_ = false;
    }

    void close_recording_camera_preview() {
        if (!camera_preview_from_recording_) return;
        camera_preview_from_recording_ = false;
        if (camera_preview_window_) {
            auto* preview_window = camera_preview_window_;
            camera_preview_window_ = nullptr;
            gtk_window_destroy(preview_window);
        }
    }

    static void on_choose_folder(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Choose recording folder");
        GFile* initial = g_file_new_for_path(self->output_directory().c_str());
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
        gtk_file_dialog_select_folder(dialog, self->window_, nullptr, on_folder_chosen, self);
        g_object_unref(dialog);
    }

    static void on_folder_chosen(GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        GError* error = nullptr;
        GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
        if (!folder) {
            g_clear_error(&error);
            return;
        }
        gchar* path = g_file_get_path(folder);
        if (path) {
            self->settings_.output_dir = path;
            gtk_label_set_text(self->output_label_, path);
            adw_action_row_set_subtitle(self->settings_output_row_, path);
            self->sync_settings();
        }
        g_free(path);
        g_object_unref(folder);
    }

    static void on_record(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->portal_) {
            self->set_status("The xdg-desktop-portal service is unavailable.");
            return;
        }
        if (choose_encoders(self->profile()).empty()) {
            self->set_status("No H.264 encoder found. Install gstreamer1-plugin-openh264.");
            return;
        }
        // If the recording includes a camera PiP, replace the standalone
        // preview with a tee branch from that same capture source. Otherwise
        // the standalone preview can keep running alongside screen capture.
        if (gtk_switch_get_active(self->camera_switch_)) self->stop_camera_preview();
        adw_dialog_close(ADW_DIALOG(self->settings_dialog_));
        self->set_controls(true);
        self->set_status("Choose a display or window in the GNOME dialog…");
        xdp_portal_create_screencast_session(
            self->portal_, static_cast<XdpOutputType>(XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW),
            XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE,
            nullptr, nullptr, on_session_created, self);
    }

    static void on_session_created(GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        GError* error = nullptr;
        self->session_ = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &error);
        if (!self->session_) {
            self->set_status(std::format("Screen sharing was cancelled: {}", error ? error->message : "unknown error"));
            g_clear_error(&error);
            self->set_controls(false);
            return;
        }
        XdpParent* parent = xdp_parent_new_gtk(self->window_);
        xdp_session_start(self->session_, parent, nullptr, on_session_started, self);
        xdp_parent_free(parent);
    }

    static void on_session_started(GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        GError* error = nullptr;
        if (!xdp_session_start_finish(XDP_SESSION(source), result, &error)) {
            self->set_status(std::format("Screen sharing was cancelled: {}", error ? error->message : "unknown error"));
            g_clear_error(&error);
            self->close_session();
            self->set_controls(false);
            return;
        }
        if (!self->start_pipeline()) {
            self->close_session();
            self->set_controls(false);
        }
    }

    bool start_pipeline(std::size_t candidate_index = 0, bool reuse_output_path = false) {
        GVariant* streams = xdp_session_get_streams(session_);
        if (!streams || g_variant_n_children(streams) == 0) {
            set_status("The portal did not provide a video stream.");
            return false;
        }
        guint32 node_id = 0;
        GVariant* properties = nullptr;
        g_variant_get_child(streams, 0, "(u@a{sv})", &node_id, &properties);
        g_variant_unref(properties);
        remote_fd_ = xdp_session_open_pipewire_remote(session_);
        if (remote_fd_ < 0) {
            set_status("Could not open the portal's PipeWire connection.");
            return false;
        }
        if (!reuse_output_path) partial_path_ = output_path(output_directory());
        if (partial_path_.empty()) {
            set_status("Could not create the Videos/Screen Recordings folder.");
            return false;
        }
        const auto active_profile = profile();
        const auto candidates = choose_encoders(active_profile);
        if (candidate_index >= candidates.size()) {
            set_status("No supported H.264 encoder is available.");
            return false;
        }
        const auto& encoder = candidates[candidate_index];
        const bool with_system_audio = gtk_switch_get_active(audio_switch_);
        const bool with_microphone = gtk_switch_get_active(microphone_switch_);
        const bool with_camera = gtk_switch_get_active(camera_switch_);
        const bool with_live_camera_preview = with_camera && settings_.camera_preview && has_element("gtk4paintablesink");
        const auto camera_device = selected_camera_device();
        if (with_camera && (camera_device.empty() || !std::filesystem::exists(camera_device))) {
            set_status("Camera overlay is enabled but the selected V4L2 camera is unavailable.");
            return false;
        }
        gchar* escaped_path = g_strescape(partial_path_.c_str(), nullptr);
        const auto source = std::format(
            "pipewiresrc fd={} path={} do-timestamp=true ! identity name=captured_counter signal-handoffs=true "
            "! queue max-size-buffers=3 max-size-time=2000000000 leaky=downstream ",
            remote_fd_, node_id);
        const auto encoder_element = named_encoder_element(encoder);
        std::string video;
        if (!with_camera && encoder.hardware) {
            // This is the normal laptop path: PipeWire negotiates DMA-BUF and
            // VA post-processing feeds Intel's low-power encoder on the GPU.
            video = std::format(
                "{}! vapostproc ! video/x-raw(memory:VAMemory),format=NV12,width={},height={},framerate={}/1 "
                "! {} ! identity name=encoded_counter signal-handoffs=true ! h264parse config-interval=-1 ! queue max-size-buffers=3 leaky=downstream ! mux. ",
                source, active_profile.width, active_profile.height, active_profile.fps, encoder_element);
        } else if (with_camera) {
            const int camera_width = active_profile.high_quality ? 1280 : active_profile.battery_saver ? 160 : 320;
            const int camera_height = active_profile.high_quality ? 720 : active_profile.battery_saver ? 90 : 180;
            const int camera_fps = active_profile.high_quality ? 30 : active_profile.battery_saver ? 5 : 10;
            const auto live_preview = sr::fedora::camera_preview_for(active_profile);
            const std::string preview_branch = with_live_camera_preview ? std::format(
                "camera_tee. ! queue max-size-buffers={} max-size-bytes=0 max-size-time=0 leaky=downstream "
                "! videoconvert ! videoscale ! videorate drop-only=true "
                "! video/x-raw,format=BGRA,width={},height={},framerate={}/1 "
                "! gtk4paintablesink name=recording_camera_preview_sink sync=false ",
                sr::fedora::kCameraPreviewQueueBuffers,
                live_preview.width, live_preview.height, live_preview.fps) : "";
            video = std::format(
                "{}! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width={},height={},framerate={}/1 "
                "! queue max-size-buffers=3 leaky=downstream ! compositor name=mix sink_1::xpos=24 sink_1::ypos=24 "
                "! videoconvert ! {} ! identity name=encoded_counter signal-handoffs=true ! h264parse config-interval=-1 ! queue ! mux. "
                "v4l2src device=\"{}\" do-timestamp=true ! queue max-size-buffers=2 leaky=downstream "
                "! videoconvert ! tee name=camera_tee "
                "camera_tee. ! queue max-size-buffers=2 leaky=downstream "
                "! videoscale ! videorate ! video/x-raw,format=I420,width={},height={},framerate={}/1 "
                "! mix. {}",
                source, active_profile.width, active_profile.height, active_profile.fps, encoder_element,
                camera_device, camera_width, camera_height, camera_fps, preview_branch);
        } else {
            // Software fallback retains the same bounded, scaled pipeline.
            video = std::format(
                "{}! videoconvert ! videoscale ! videorate ! video/x-raw,format=I420,width={},height={},framerate={}/1 "
                "! {} ! identity name=encoded_counter signal-handoffs=true ! h264parse config-interval=-1 ! queue max-size-buffers=3 leaky=downstream ! mux. ",
                source, active_profile.width, active_profile.height, active_profile.fps, encoder_element);
        }
        std::string audio;
        if (with_system_audio || with_microphone) {
            audio = "audiomixer name=audio_mix ! audioconvert ! audioresample ! "
                    "audio/x-raw,format=F32LE,rate=48000,channels=2 ! volume name=audio_volume ! "
                    "avenc_aac bitrate=128000 ! aacparse ! identity name=audio_counter signal-handoffs=true ! queue ! mux. ";
            if (with_system_audio) {
                // The Pulse compatibility monitor is the PipeWire desktop-output
                // loopback; the ordinary default source would be the microphone.
                audio += "pulsesrc device=@DEFAULT_MONITOR@ do-timestamp=true ! queue max-size-buffers=12 "
                         "max-size-time=2000000000 leaky=downstream ! audioconvert ! audioresample ! audio_mix. ";
            }
            if (with_microphone) {
                audio += "pulsesrc do-timestamp=true ! queue max-size-buffers=12 max-size-time=2000000000 leaky=downstream "
                         "! audioconvert ! audioresample ! audiodynamic mode=expander threshold=0.02 ratio=4 "
                         "characteristics=soft-knee ! audio_mix. ";
            }
        }
        const std::string description = std::format(
            "mp4mux name=mux faststart=true ! filesink location=\"{}\" {}{}", escaped_path, video, audio);
        g_free(escaped_path);
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(description.c_str(), &error);
        if (!pipeline_) {
            const auto detail = std::string{error ? error->message : "unknown error"};
            g_clear_error(&error);
            close(remote_fd_);
            remote_fd_ = -1;
            if (candidate_index + 1 < candidates.size()) {
                set_status(std::format("{} was unavailable; retrying {}…", encoder.name, candidates[candidate_index + 1].name));
                return start_pipeline(candidate_index + 1, true);
            }
            set_status(std::format("Could not create recording pipeline: {}", detail));
            return false;
        }
        captured_frames_.store(0, std::memory_order_relaxed);
        encoded_frames_.store(0, std::memory_order_relaxed);
        audio_buffers_.store(0, std::memory_order_relaxed);
        qos_drops_.store(0, std::memory_order_relaxed);
        if (GstElement* counter = gst_bin_get_by_name(GST_BIN(pipeline_), "captured_counter")) {
            g_signal_connect(counter, "handoff", G_CALLBACK(on_captured_handoff), this);
            gst_object_unref(counter);
        }
        if (GstElement* counter = gst_bin_get_by_name(GST_BIN(pipeline_), "encoded_counter")) {
            g_signal_connect(counter, "handoff", G_CALLBACK(on_encoded_handoff), this);
            gst_object_unref(counter);
        }
        if (GstElement* counter = gst_bin_get_by_name(GST_BIN(pipeline_), "audio_counter")) {
            g_signal_connect(counter, "handoff", G_CALLBACK(on_audio_handoff), this);
            gst_object_unref(counter);
        }
        GstBus* bus = gst_element_get_bus(pipeline_);
        bus_watch_ = gst_bus_add_watch(bus, on_bus_message, this);
        gst_object_unref(bus);
        if (with_live_camera_preview) {
            GstElement* preview_sink = gst_bin_get_by_name(
                GST_BIN(pipeline_), "recording_camera_preview_sink");
            const bool preview_opened = preview_sink && open_camera_preview_window(preview_sink, true);
            if (preview_sink) gst_object_unref(preview_sink);
            if (!preview_opened) {
                // Recording is still valid without a GTK paintable. Keep the
                // camera PiP path running and report the degraded UI state.
                set_status("Camera preview is unavailable; recording will continue with camera PiP.");
            }
        }
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            const auto failed_encoder = encoder.name;
            cleanup_pipeline();
            if (candidate_index + 1 < candidates.size()) {
                set_status(std::format("{} could not start; retrying {}…", failed_encoder, candidates[candidate_index + 1].name));
                return start_pipeline(candidate_index + 1, true);
            }
            set_status("GStreamer could not start the recording pipeline.");
            return false;
        }
        recording_clock_.start(std::chrono::steady_clock::now());
        recording_ = true;
        muted_ = false;
        active_profile_ = active_profile;
        active_encoder_ = encoder;
        active_candidate_index_ = candidate_index;
        active_candidate_count_ = candidates.size();
        active_audio_ = with_system_audio || with_microphone;
        active_camera_ = with_camera;
        write_diagnostics("START", active_profile, encoder, with_system_audio || with_microphone, with_camera);
        set_status(std::format("Recording to {} ({})", std::filesystem::path(final_path_for(partial_path_)).filename().string(), encoder.name));
        timer_ = g_timeout_add(250, update_timer, this);
        disk_check_ = g_timeout_add_seconds(10, check_disk_space, this);
        power_check_ = g_timeout_add_seconds(10, check_power_state, this);
        return true;
    }

    void write_diagnostics(const char* phase, const RecordingProfile& active_profile,
                           const EncoderChoice& encoder, bool with_audio, bool with_camera) const {
        std::ofstream file(partial_path_ + ".diagnostics.txt", std::ios::app);
        if (!file) return;
        file << phase << '\n'
             << "encoder=" << encoder.name << (encoder.hardware ? " (hardware)" : " (software)") << '\n'
             << "power=" << (active_profile.on_ac ? "AC" : "battery") << '\n'
             << "profile=" << (active_profile.high_quality ? "high_quality" : active_profile.battery_saver ? "battery_saver" : "efficiency") << '\n'
             << "battery_saver=" << (active_profile.battery_saver ? "on" : "off") << '\n'
             << "video=" << active_profile.width << 'x' << active_profile.height << '@' << active_profile.fps
             << " bitrate_kbps=" << active_profile.bitrate_kbps << '\n'
             << "system_audio=" << (with_audio ? "on" : "off") << " camera_overlay=" << (with_camera ? "on" : "off") << '\n';
        if (with_camera) file << "camera_device=" << selected_camera_device() << '\n';
    }

    void write_stop_diagnostics(bool completed) const {
        std::ofstream file(partial_path_ + ".diagnostics.txt", std::ios::app);
        if (!file) return;
        const auto snapshot = telemetry_snapshot();
        file << "STOP\nstatus=" << (completed ? "completed" : "partial") << '\n'
             << "captured_frames=" << snapshot.captured_frames << '\n'
             << "encoded_frames=" << snapshot.encoded_frames << '\n'
             << "audio_buffers=" << snapshot.audio_buffers << '\n'
             << "qos_drops=" << snapshot.qos_drops << '\n';
    }

    void write_fault_diagnostics(const sr::fedora::RecordingFault& fault, std::string_view source_name,
                                 std::string_view detail) const {
        std::ofstream file(partial_path_ + ".diagnostics.txt", std::ios::app);
        if (!file) return;
        file << "FAULT\nkind=" << fault.diagnostic_name << '\n'
             << "source=" << source_name << '\n'
             << "detail=" << detail << '\n';
    }

    static gboolean check_disk_space(gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        std::error_code error;
        const auto space = std::filesystem::space(std::filesystem::path(self->partial_path_).parent_path(), error);
        if (!error && space.available < 500ULL * 1024ULL * 1024ULL) {
            self->set_status("Less than 500 MB free — stopping safely to protect the recording.");
            on_stop(nullptr, self);
            return G_SOURCE_REMOVE;
        }
        return self->recording_ ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

    static gboolean check_power_state(gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->recording_) return G_SOURCE_REMOVE;
        const auto updated = self->profile();
        if (updated.on_ac == self->active_profile_.on_ac) return G_SOURCE_CONTINUE;

        // The active portal stream keeps its negotiated resolution/FPS to avoid
        // an MP4 discontinuity. The encoder bitrate is mutable, so efficiency
        // mode immediately applies the battery cap; the next session receives
        // the full negotiated battery profile including 15 FPS.
        if (!updated.high_quality && self->pipeline_ && self->active_encoder_) {
            if (GstElement* encoder = gst_bin_get_by_name(GST_BIN(self->pipeline_), "video_encoder")) {
                const int bitrate = self->active_encoder_->hardware ? updated.bitrate_kbps : updated.bitrate_kbps * 1000;
                g_object_set(encoder, "bitrate", bitrate, nullptr);
                gst_object_unref(encoder);
            }
        }
        self->active_profile_ = updated;
        self->write_diagnostics("POWER_CHANGE", updated,
                                self->active_encoder_.value_or(EncoderChoice{}), self->active_audio_, self->active_camera_);
        self->set_status(std::format("Power changed to {}; active encoder bitrate is now {} Mbps.",
            updated.on_ac ? "AC" : "battery", updated.bitrate_kbps / 1000));
        return G_SOURCE_CONTINUE;
    }

    static gboolean on_bus_message(GstBus*, GstMessage* message, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_EOS:
                self->bus_watch_ = 0;
                self->finish_recording(true);
                return G_SOURCE_REMOVE;
            case GST_MESSAGE_ERROR: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                const auto* source = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
                const auto source_name = std::string{source ? source : "unknown"};
                const auto fault = sr::fedora::classify_recording_fault(source_name);
                const auto detail = std::string{error ? error->message : "unknown error"};
                const auto retry_encoder = source_name == "video_encoder" && !self->stopping_ &&
                    sr::fedora::should_retry_encoder_startup(
                        self->encoded_frames_.load(std::memory_order_relaxed),
                        self->active_candidate_index_, self->active_candidate_count_);
                if (retry_encoder) {
                    const auto next_index = self->active_candidate_index_ + 1;
                    const auto failed_encoder = self->active_encoder_.value_or(EncoderChoice{}).name;
                    g_clear_error(&error);
                    g_free(debug);
                    self->bus_watch_ = 0;
                    self->recording_ = false;
                    self->cleanup_pipeline();
                    self->set_status(std::format("{} failed before the first frame; trying the next H.264 encoder…", failed_encoder));
                    if (!self->start_pipeline(next_index, true)) {
                        self->close_session();
                        self->set_controls(false);
                    }
                    return G_SOURCE_REMOVE;
                }
                self->write_fault_diagnostics(fault, source_name, detail);
                g_clear_error(&error);
                g_free(debug);
                self->bus_watch_ = 0;
                self->finish_recording(false, fault.user_message);
                return G_SOURCE_REMOVE;
            }
            case GST_MESSAGE_QOS: {
                guint64 processed = 0;
                guint64 dropped = 0;
                gst_message_parse_qos_stats(message, nullptr, &processed, &dropped);
                self->qos_drops_.store(dropped, std::memory_order_relaxed);
                return G_SOURCE_CONTINUE;
            }
            default:
                return G_SOURCE_CONTINUE;
        }
    }

    static gboolean update_timer(gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->recording_) return G_SOURCE_REMOVE;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            self->recording_clock_.elapsed(std::chrono::steady_clock::now()));
        gtk_label_set_text(self->time_label_, std::format("{:02}:{:02}:{:02}", seconds.count() / 3600, (seconds.count() / 60) % 60, seconds.count() % 60).c_str());
        gtk_label_set_text(self->telemetry_label_, sr::fedora::format_telemetry(self->telemetry_snapshot()).c_str());
        return G_SOURCE_CONTINUE;
    }

    sr::fedora::TelemetrySnapshot telemetry_snapshot() const {
        return {captured_frames_.load(std::memory_order_relaxed),
                encoded_frames_.load(std::memory_order_relaxed),
                audio_buffers_.load(std::memory_order_relaxed),
                qos_drops_.load(std::memory_order_relaxed)};
    }

    static void on_captured_handoff(GstElement*, GstBuffer*, gpointer data) {
        static_cast<RecorderWindow*>(data)->captured_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    static void on_encoded_handoff(GstElement*, GstBuffer*, gpointer data) {
        static_cast<RecorderWindow*>(data)->encoded_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    static void on_audio_handoff(GstElement*, GstBuffer*, gpointer data) {
        static_cast<RecorderWindow*>(data)->audio_buffers_.fetch_add(1, std::memory_order_relaxed);
    }

    static void on_pause(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->pipeline_) return;
        if (!self->recording_clock_.paused()) {
            gst_element_set_state(self->pipeline_, GST_STATE_PAUSED);
            self->recording_clock_.pause(std::chrono::steady_clock::now());
            gtk_button_set_label(self->pause_button_, "Resume");
            self->set_status("Recording paused");
        } else {
            self->recording_clock_.resume(std::chrono::steady_clock::now());
            gst_element_set_state(self->pipeline_, GST_STATE_PLAYING);
            gtk_button_set_label(self->pause_button_, "Pause");
            self->set_status("Recording resumed");
        }
    }

    static void on_mute(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        self->muted_ = !self->muted_;
        if (self->pipeline_) {
            if (GstElement* volume = gst_bin_get_by_name(GST_BIN(self->pipeline_), "audio_volume")) {
                g_object_set(volume, "volume", self->muted_ ? 0.0 : 1.0, nullptr);
                gst_object_unref(volume);
            }
        }
        gtk_button_set_label(self->mute_button_, self->muted_ ? "Unmute" : "Mute");
    }

    static void on_stop(GtkButton*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (!self->pipeline_ || self->stopping_) return;
        self->stopping_ = true;
        gtk_widget_set_sensitive(GTK_WIDGET(self->stop_button_), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(self->pause_button_), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(self->mute_button_), FALSE);
        self->set_status("Finalizing MP4…");
        gst_element_send_event(self->pipeline_, gst_event_new_eos());
    }

    void finish_recording(bool completed, std::string_view failure_message = {}) {
        const auto partial = partial_path_;
        write_stop_diagnostics(completed);
        cleanup_pipeline();
        close_session();
        recording_ = false;
        stopping_ = false;
        set_controls(false);
        gtk_label_set_text(time_label_, "00:00:00");
        if (completed && std::filesystem::exists(partial)) {
            const auto final = final_path_for(partial);
            std::error_code error;
            std::filesystem::rename(partial, final, error);
            if (!error) {
                std::error_code diagnostics_error;
                std::filesystem::rename(partial + ".diagnostics.txt", final + ".diagnostics.txt", diagnostics_error);
            }
            set_status(error ? std::format("Recording kept as {}", partial) : std::format("Saved {}", final));
        } else if (std::filesystem::exists(partial)) {
            set_status(failure_message.empty() ? std::format("Recording kept as {}", partial)
                                               : std::format("{} Recording preserved as {}", failure_message, partial));
        } else if (!failure_message.empty()) {
            set_status(std::string{failure_message});
        }
    }

    void cleanup_pipeline() {
        if (timer_) {
            g_source_remove(timer_);
            timer_ = 0;
        }
        if (disk_check_) {
            g_source_remove(disk_check_);
            disk_check_ = 0;
        }
        if (power_check_) {
            g_source_remove(power_check_);
            power_check_ = 0;
        }
        if (bus_watch_) {
            g_source_remove(bus_watch_);
            bus_watch_ = 0;
        }
        // The GTK paintable is owned by a sink in pipeline_. Destroy its
        // window before releasing the pipeline at the end of a recording.
        close_recording_camera_preview();
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        if (remote_fd_ >= 0) {
            close(remote_fd_);
            remote_fd_ = -1;
        }
    }

    void close_session() {
        if (session_) {
            xdp_session_close(session_);
            g_object_unref(session_);
            session_ = nullptr;
        }
    }

    static gboolean on_close(GtkWindow*, gpointer data) {
        auto* self = static_cast<RecorderWindow*>(data);
        if (self->recording_ && !self->stopping_) {
            on_stop(nullptr, self);
            return TRUE;
        }
        return FALSE;
    }
};

void activate(GApplication* application, gpointer) {
    if (auto* existing = static_cast<RecorderWindow*>(g_object_get_data(G_OBJECT(application), "recorder-window"))) {
        existing->present();
        return;
    }
    auto* window = new RecorderWindow(ADW_APPLICATION(application));
    g_object_set_data_full(G_OBJECT(application), "recorder-window", window,
                           [](gpointer data) { delete static_cast<RecorderWindow*>(data); });
    window->present();
}

}  // namespace

int main(int argc, char** argv) {
    // GStreamer's shared registry can predate a newly installed Intel media
    // driver. Keep a recorder-owned registry so hardware codec discovery is
    // deterministic and never depends on an unrelated desktop app's cache.
    if (!g_getenv("GST_REGISTRY")) {
        const auto registry_dir = std::filesystem::path(g_get_user_cache_dir()) / "fedora-screen-recorder";
        std::error_code error;
        std::filesystem::create_directories(registry_dir, error);
        if (!error) {
            g_setenv("GST_REGISTRY", (registry_dir / "gstreamer-registry.bin").c_str(), FALSE);
        }
    }
    gst_init(&argc, &argv);
    gst_update_registry();
    const auto application_flags = g_getenv("SCREEN_RECORDER_UI_SMOKE")
        ? G_APPLICATION_NON_UNIQUE
        : G_APPLICATION_DEFAULT_FLAGS;
    auto* application = adw_application_new(kAppId, application_flags);
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    const int result = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return result;
}
