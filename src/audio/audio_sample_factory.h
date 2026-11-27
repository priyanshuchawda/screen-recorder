#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

namespace sr {

// Creates a new sample and buffer for one downstream submission. The caller
// retains its own references; the sink writer may retain the sample safely.
inline HRESULT create_audio_sample(DWORD capacity,
                                   Microsoft::WRL::ComPtr<IMFSample>* sample_out,
                                   Microsoft::WRL::ComPtr<IMFMediaBuffer>* buffer_out) {
    if (!sample_out || !buffer_out) return E_POINTER;
    sample_out->Reset();
    buffer_out->Reset();
    if (capacity == 0) return E_INVALIDARG;

    HRESULT hr = MFCreateSample(sample_out->GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = MFCreateMemoryBuffer(capacity, buffer_out->GetAddressOf());
    if (FAILED(hr)) {
        sample_out->Reset();
        return hr;
    }

    hr = (*sample_out)->AddBuffer(buffer_out->Get());
    if (FAILED(hr)) {
        buffer_out->Reset();
        sample_out->Reset();
    }
    return hr;
}

} // namespace sr
