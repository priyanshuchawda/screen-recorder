#include <gtest/gtest.h>

#include <mfapi.h>
#include <wrl/client.h>
#include "audio/audio_sample_factory.h"

using Microsoft::WRL::ComPtr;

TEST(AudioSampleFactoryTest, CreatesIndependentlyOwnedSampleAndBuffer) {
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    const HRESULT hr = sr::create_audio_sample(1920, &sample, &buffer);
    ASSERT_TRUE(SUCCEEDED(hr));
    ASSERT_NE(sample.Get(), nullptr);
    ASSERT_NE(buffer.Get(), nullptr);

    DWORD buffer_count = 0;
    ASSERT_TRUE(SUCCEEDED(sample->GetBufferCount(&buffer_count)));
    EXPECT_EQ(buffer_count, 1u);
    ComPtr<IMFMediaBuffer> attached;
    ASSERT_TRUE(SUCCEEDED(sample->GetBufferByIndex(0, &attached)));
    EXPECT_EQ(attached.Get(), buffer.Get());
    DWORD max_length = 0;
    ASSERT_TRUE(SUCCEEDED(buffer->GetMaxLength(&max_length)));
    EXPECT_GE(max_length, 1920u);

    sample.Reset();
    buffer.Reset();
    EXPECT_TRUE(SUCCEEDED(MFShutdown()));
}
