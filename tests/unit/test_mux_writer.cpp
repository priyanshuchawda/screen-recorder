#include <gtest/gtest.h>

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
