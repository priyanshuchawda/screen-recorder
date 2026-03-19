#include <windows.h>
#include <iostream>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

using namespace winrt;
using namespace Windows::Graphics::Capture;

// Minimal prototype for Phase 1: Core setup verification
int main()
{
    // Step 1: Initialize COM, explicitly requesting MTA (Multi-Threaded Apartment)
    // Necessary for both C++/WinRT Graphics Capture and Media Foundation
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wcout << L"[OK] COM Multi-Threaded Apartment initialized." << std::endl;

    // Step 2: Initialize Media Foundation
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        std::wcerr << L"[ERROR] Failed to initialize Media Foundation: 0x" << std::hex << hr << std::endl;
        return -1;
    }
    // wil::scope_exit automatically cleans up Media Foundation when this block exits
    auto mfShutdown = wil::scope_exit([] { MFShutdown(); std::wcout << L"[INFO] Media Foundation shut down." << std::endl; });
    std::wcout << L"[OK] Media Foundation initialized." << std::endl;

    // Step 3: Check if Windows Graphics Capture is natively supported
    bool isCaptureSupported = GraphicsCaptureSession::IsSupported();
    if (!isCaptureSupported)
    {
        std::wcerr << L"[ERROR] Windows Graphics Capture is NOT supported on this system." << std::endl;
        return -1;
    }
    std::wcout << L"[OK] Windows Graphics Capture is supported." << std::endl;

    // Step 4: Dummy check for Direct3D11 setup, simulating what we need for the engine
    wil::com_ptr<ID3D11Device> d3dDevice;
    wil::com_ptr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel;
    
    hr = D3D11CreateDevice(
        nullptr, // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, // Required for Direct2D/WinRT interop
        nullptr, 0,
        D3D11_SDK_VERSION,
        &d3dDevice,
        &featureLevel,
        &d3dContext
    );

    if (SUCCEEDED(hr))
    {
        std::wcout << L"[OK] Direct3D 11 Hardware device created successfully." << std::endl;
    }
    else
    {
        std::wcerr << L"[ERROR] Failed to create D3D11 Device: 0x" << std::hex << hr << std::endl;
        return -1;
    }

    std::wcout << L"\n======================================================\n";
    std::wcout << L"Setup complete! Phase 1 foundation is ready." << std::endl;
    std::wcout << L"You can now begin implementing the Capture Engine." << std::endl;
    std::wcout << L"======================================================\n\n";

    return 0;
}
