#pragma once
// encoder_probe.h — D3D11 device creation and hardware H.264 encoder enumeration
// T008: Setup D3D11 device and HW Encoder enumeration

#include <d3d11.h>
#include <d3d11_1.h>   // ID3D11Multithread
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <string>

namespace sr {

using Microsoft::WRL::ComPtr;

struct ProbeResult {
    ComPtr<ID3D11Device>         d3d_device;
    ComPtr<ID3D11DeviceContext>  d3d_context;
    ComPtr<IDXGIAdapter>         adapter;
    ComPtr<IMFDXGIDeviceManager> dxgi_device_manager;
    UINT                         reset_token = 0;
    bool                         hw_encoder_available = false;
    std::wstring                 encoder_name;
    std::wstring                 adapter_name;
};

class EncoderProbe {
public:
    // Create D3D11 device + enumerate hardware H.264 encoders
    // Returns false if D3D11 device creation fails
    static bool run(ProbeResult& result);
};

} // namespace sr
