// encoder_probe.cpp — D3D11 device creation and HW encoder enumeration
#include "encoder/encoder_probe.h"
#include "utils/logging.h"

#include <d3d11_1.h>    // ID3D11Multithread
#include <codecapi.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sr {

bool EncoderProbe::run(ProbeResult& result) {
    // --- D3D11 Device Creation ---
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actual_level{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        &result.d3d_device,
        &actual_level,
        &result.d3d_context
    );

    if (FAILED(hr)) {
        // Retry without debug flag (debug layer may not be installed)
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, feature_levels, 2,
            D3D11_SDK_VERSION,
            &result.d3d_device, &actual_level, &result.d3d_context);
    }

    if (FAILED(hr)) {
        SR_LOG_ERROR(L"D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Make the D3D device multi-thread safe (required for Video Processor)
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(result.d3d_device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    // --- Adapter name ---
    ComPtr<IDXGIDevice> dxgi_device;
    result.d3d_device.As(&dxgi_device);
    dxgi_device->GetAdapter(&result.adapter);

    DXGI_ADAPTER_DESC adapter_desc{};
    result.adapter->GetDesc(&adapter_desc);
    result.adapter_name = adapter_desc.Description;
    SR_LOG_INFO(L"D3D11 adapter: %s", adapter_desc.Description);

    // --- DXGI Device Manager (for sharing D3D device with MFTs) ---
    hr = MFCreateDXGIDeviceManager(&result.reset_token, &result.dxgi_device_manager);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"MFCreateDXGIDeviceManager failed: 0x%08X", hr);
        return false;
    }

    hr = result.dxgi_device_manager->ResetDevice(result.d3d_device.Get(), result.reset_token);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"IMFDXGIDeviceManager::ResetDevice failed: 0x%08X", hr);
        return false;
    }

    // --- Hardware H.264 Encoder Enumeration ---
    MFT_REGISTER_TYPE_INFO output_type{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &output_type,
        &activates,
        &count
    );

    if (SUCCEEDED(hr) && count > 0) {
        WCHAR name_buf[256]{};
        activates[0]->GetString(MFT_FRIENDLY_NAME_Attribute, name_buf, 256, nullptr);
        result.encoder_name        = name_buf;
        result.hw_encoder_available = true;
        SR_LOG_INFO(L"HW encoder found: %s", name_buf);
    } else {
        result.hw_encoder_available = false;
        SR_LOG_INFO(L"No hardware H.264 encoder — will use software fallback");
    }

    if (activates) {
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
    }

    return true;
}

} // namespace sr
