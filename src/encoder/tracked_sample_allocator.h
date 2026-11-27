#pragma once

#include "utils/render_frame.h"

#include <mfidl.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>

namespace sr {

class SurfaceLeaseState final : public IUnknown {
public:
    explicit SurfaceLeaseState(std::shared_ptr<TextureSurfaceLease> lease)
        : lease_(std::move(lease)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<TextureSurfaceLease> lease_;
};

class SurfaceLeaseCallback final : public IMFAsyncCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
        if (flags) *flags = 0;
        if (queue) *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
        if (!result) return E_POINTER;
        Microsoft::WRL::ComPtr<IUnknown> state;
        return result->GetState(&state);
    }

private:
    std::atomic<ULONG> refs_{1};
};

inline HRESULT create_tracked_surface_sample(
    std::shared_ptr<TextureSurfaceLease> lease,
    Microsoft::WRL::ComPtr<IMFSample>* sample_out)
{
    if (!sample_out || !lease) return E_INVALIDARG;
    sample_out->Reset();

    Microsoft::WRL::ComPtr<IMFTrackedSample> tracked;
    HRESULT hr = MFCreateTrackedSample(&tracked);
    if (FAILED(hr)) return hr;
    hr = tracked.As(sample_out);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMFAsyncCallback> callback;
    callback.Attach(new (std::nothrow) SurfaceLeaseCallback());
    if (!callback) {
        sample_out->Reset();
        return E_OUTOFMEMORY;
    }
    Microsoft::WRL::ComPtr<IUnknown> state;
    state.Attach(new (std::nothrow) SurfaceLeaseState(std::move(lease)));
    if (!state) {
        sample_out->Reset();
        return E_OUTOFMEMORY;
    }
    hr = tracked->SetAllocator(callback.Get(), state.Get());
    if (FAILED(hr)) sample_out->Reset();
    return hr;
}

} // namespace sr
