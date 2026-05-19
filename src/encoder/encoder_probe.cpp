// encoder_probe.cpp — D3D11 device creation and HW encoder enumeration
#include "encoder/encoder_probe.h"
#include "utils/logging.h"

#include <d3d11_1.h>    // ID3D11Multithread
#include <codecapi.h>
#include <algorithm>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace sr {

namespace {

constexpr UINT kIntelVendorId = 0x8086;

struct AdapterCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    int score = 0;
    UINT ordinal = 0;
};

HRESULT create_device(IDXGIAdapter* adapter,
                      UINT flags,
                      ID3D11Device** device,
                      D3D_FEATURE_LEVEL* actual_level,
                      ID3D11DeviceContext** context) {
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    return D3D11CreateDevice(
        adapter,
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        device,
        actual_level,
        context);
}

HRESULT create_device_with_debug_fallback(IDXGIAdapter* adapter,
                                          UINT flags,
                                          ComPtr<ID3D11Device>& device,
                                          D3D_FEATURE_LEVEL& actual_level,
                                          ComPtr<ID3D11DeviceContext>& context) {
    device.Reset();
    context.Reset();

    HRESULT hr = create_device(adapter, flags, &device, &actual_level, &context);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    if ((flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        device.Reset();
        context.Reset();
        const UINT fallback_flags = flags & ~D3D11_CREATE_DEVICE_DEBUG;
        hr = create_device(adapter, fallback_flags, &device, &actual_level, &context);
    }

    return hr;
}

bool create_preferred_d3d_device(ProbeResult& result, UINT flags) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        SR_LOG_WARN(L"CreateDXGIFactory1 failed: 0x%08X — using default D3D adapter", hr);
        D3D_FEATURE_LEVEL actual_level{};
        hr = create_device_with_debug_fallback(nullptr, flags, result.d3d_device,
                                               actual_level, result.d3d_context);
        return SUCCEEDED(hr);
    }

    std::vector<AdapterCandidate> candidates;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }

        const bool is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        candidates.push_back(AdapterCandidate{
            adapter,
            desc,
            EncoderProbe::adapter_preference_score(desc.VendorId, is_software),
            i
        });
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const AdapterCandidate& lhs, const AdapterCandidate& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.ordinal < rhs.ordinal;
        });

    D3D_FEATURE_LEVEL actual_level{};
    for (const auto& candidate : candidates) {
        if (candidate.score < 0) {
            continue;
        }

        hr = create_device_with_debug_fallback(candidate.adapter.Get(), flags,
                                               result.d3d_device, actual_level,
                                               result.d3d_context);
        if (SUCCEEDED(hr)) {
            SR_LOG_INFO(L"D3D11 adapter selected for HW encoding: %s (vendor=0x%04X%s)",
                        candidate.desc.Description,
                        candidate.desc.VendorId,
                        candidate.desc.VendorId == kIntelVendorId ? L", Intel preferred" : L"");
            return true;
        }

        SR_LOG_WARN(L"D3D11 adapter candidate failed: %s (vendor=0x%04X, hr=0x%08X)",
                    candidate.desc.Description, candidate.desc.VendorId, hr);
    }

    SR_LOG_WARN(L"No enumerated hardware adapter created a D3D11 video device — using default D3D adapter");
    hr = create_device_with_debug_fallback(nullptr, flags, result.d3d_device,
                                           actual_level, result.d3d_context);
    return SUCCEEDED(hr);
}

} // namespace

int EncoderProbe::adapter_preference_score(UINT vendor_id, bool is_software) noexcept {
    if (is_software) {
        return -1;
    }
    return vendor_id == kIntelVendorId ? 100 : 10;
}

bool EncoderProbe::run(ProbeResult& result) {
    // --- D3D11 Device Creation ---
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    if (!create_preferred_d3d_device(result, flags)) {
        SR_LOG_ERROR(L"D3D11CreateDevice failed on all adapter candidates");
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
    HRESULT hr = MFCreateDXGIDeviceManager(&result.reset_token, &result.dxgi_device_manager);
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
