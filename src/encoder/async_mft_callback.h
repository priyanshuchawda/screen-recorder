#pragma once

#include <mfidl.h>
#include <mferror.h>
#include <wrl/implements.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace sr {

// Completes Media Foundation's one outstanding BeginGetEvent request, copies
// only its small control event into the scheduler, then rearms. It never calls
// IMFTransform methods; the encoder worker is the sole transform owner.
template <typename Scheduler>
class MftAsyncEventCallback final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAsyncCallback> {
public:
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
        if (!result) return E_POINTER;
        active_callbacks_.fetch_add(1, std::memory_order_acq_rel);
        pending_request_.store(false, std::memory_order_release);
        notify_quiescent();

        Microsoft::WRL::ComPtr<IMFMediaEvent> event;
        Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator = generator_;
        HRESULT hr = generator ? generator->EndGetEvent(result, &event)
                                : MF_E_SHUTDOWN;
        bool terminal = false;
        if (SUCCEEDED(hr)) {
            MediaEventType type = MEUnknown;
            HRESULT event_status = S_OK;
            hr = event->GetType(&type);
            if (SUCCEEDED(hr)) hr = event->GetStatus(&event_status);
            if (SUCCEEDED(hr) && FAILED(event_status)) hr = event_status;

            if (SUCCEEDED(hr)) {
                switch (type) {
                case METransformNeedInput: {
                    UINT32 stream_id = 0;
                    hr = event->GetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, &stream_id);
                    if (SUCCEEDED(hr)) scheduler_->grant_input(stream_id);
                    break;
                }
                case METransformHaveOutput:
                    scheduler_->grant_output();
                    break;
                case METransformDrainComplete:
                    scheduler_->signal_drain_complete();
                    terminal = true;
                    break;
                case MEError:
                    hr = event_status;
                    if (SUCCEEDED(hr)) hr = E_FAIL;
                    break;
                default:
                    break;
                }
            }
        }
        if (FAILED(hr)) {
            if (!closing_.load(std::memory_order_acquire)) scheduler_->fail(hr);
            terminal = true;
        }
        if (terminal) closing_.store(true, std::memory_order_release);

        HRESULT arm_hr = S_OK;
        if (!closing_.load(std::memory_order_acquire)) {
            arm_hr = begin_get_event();
            if (FAILED(arm_hr)) {
                scheduler_->fail(arm_hr);
                closing_.store(true, std::memory_order_release);
            }
        }
        if (closing_.load(std::memory_order_acquire)) {
            std::lock_guard lock(begin_mutex_);
            generator_.Reset();
        }

        active_callbacks_.fetch_sub(1, std::memory_order_acq_rel);
        notify_quiescent();
        return FAILED(hr) ? hr : arm_hr;
    }

    HRESULT start(IMFMediaEventGenerator* generator, Scheduler* scheduler) {
        if (!generator || !scheduler) return E_POINTER;
        generator_ = generator;
        scheduler_ = scheduler;
        closing_.store(false, std::memory_order_release);
        return begin_get_event();
    }

    void disarm() noexcept {
        std::lock_guard lock(begin_mutex_);
        closing_.store(true, std::memory_order_release);
        notify_quiescent();
    }

    bool wait_quiescent(DWORD timeout_ms) {
        std::unique_lock lock(quiescent_mutex_);
        return quiescent_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return !pending_request_.load(std::memory_order_acquire) &&
                   active_callbacks_.load(std::memory_order_acquire) == 0;
        });
    }

private:
    void notify_quiescent() {
        std::lock_guard lock(quiescent_mutex_);
        quiescent_cv_.notify_all();
    }

    HRESULT begin_get_event() {
        std::lock_guard lock(begin_mutex_);
        if (closing_.load(std::memory_order_acquire)) return S_FALSE;
        if (pending_request_.exchange(true, std::memory_order_acq_rel)) {
            return MF_E_MULTIPLE_BEGIN;
        }
        const HRESULT hr = generator_ ? generator_->BeginGetEvent(this, nullptr) : MF_E_SHUTDOWN;
        if (FAILED(hr)) {
            pending_request_.store(false, std::memory_order_release);
            notify_quiescent();
        }
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator_;
    Scheduler* scheduler_ = nullptr;
    std::atomic<bool> closing_{ true };
    std::atomic<bool> pending_request_{ false };
    std::atomic<uint32_t> active_callbacks_{ 0 };
    std::mutex begin_mutex_;
    std::mutex quiescent_mutex_;
    std::condition_variable quiescent_cv_;
};

} // namespace sr
