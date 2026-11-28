#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <windows.h>
#include <mfapi.h>

#include "storage/mux_writer.h"

TEST(MuxWriterConfigTest, KeepsSinkWriterThrottlingEnabled) {
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    ASSERT_TRUE(SUCCEEDED(MFCreateAttributes(&attrs, 4)));

    ASSERT_TRUE(SUCCEEDED(sr::configure_mux_writer_attributes(attrs.Get())));

    UINT32 hardware_transforms = FALSE;
    EXPECT_TRUE(SUCCEEDED(attrs->GetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                           &hardware_transforms)));
    EXPECT_EQ(hardware_transforms, TRUE);

    UINT32 disable_throttling = TRUE;
    const HRESULT hr = attrs->GetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING,
                                        &disable_throttling);
    if (SUCCEEDED(hr)) {
        EXPECT_EQ(disable_throttling, FALSE);
    }
}

TEST(MuxWriterStartupTest, CreatesSinkWriterOnItsDedicatedWorker) {
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));

    wchar_t temp_dir[MAX_PATH]{};
    ASSERT_NE(GetTempPathW(MAX_PATH, temp_dir), 0u);
    const std::wstring partial_path = std::wstring(temp_dir) +
        L"ScreenRecorder-M3-" + std::to_wstring(GetCurrentProcessId()) + L".partial.mp4";
    const std::wstring final_path = partial_path.substr(0, partial_path.size() -
        std::wstring(L".partial.mp4").size()) + L".mp4";
    DeleteFileW(partial_path.c_str());
    DeleteFileW(final_path.c_str());

    sr::MuxWriter writer;
    sr::MuxConfig config;
    config.video_width = 640;
    config.video_height = 360;
    config.video_fps_num = 30;
    config.video_fps_den = 1;
    config.video_bitrate = 1'000'000;
    config.audio_sample_rate = 48'000;
    config.audio_channels = 2;
    config.audio_bits_per_sample = 32;
    config.audio_is_float = true;

    const bool initialized = writer.initialize(partial_path, final_path, config);
    EXPECT_TRUE(initialized);
    EXPECT_TRUE(writer.initialized());
    writer.abort();

    DeleteFileW(partial_path.c_str());
    EXPECT_TRUE(SUCCEEDED(MFShutdown()));
}
