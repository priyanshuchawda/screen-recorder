// capture_engine.cpp — Windows Graphics Capture + GPU BGRA->NV12 via D3D11 Video Processor
// T010/T011: WGC free-threaded frame pool, BGRA->NV12 conversion, push to BoundedQueue

// WinRT / WGC includes (kept in .cpp to isolate from header via PIMPL)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "capture/capture_engine.h"
#include "utils/logging.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wdx  = winrt::Windows::Graphics::DirectX;
namespace wdx3 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace sr {

// ---------------------------------------------------------------------------
// PIMPL implementation struct: holds all WinRT/D3D objects
// ---------------------------------------------------------------------------
struct CaptureEngineImpl {
    // D3D objects (borrowed — not owned)
    ID3D11Device*        d3d_device  = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;

    // WinRT device wrapper
    wdx3::IDirect3DDevice winrt_device{ nullptr };

    // WGC objects
    wgc::GraphicsCaptureItem        item      { nullptr };
    wgc::Direct3D11CaptureFramePool frame_pool{ nullptr };
    wgc::GraphicsCaptureSession     session   { nullptr };
    winrt::event_token              frame_token{};

    // D3D11 Video Processor for BGRA->NV12
    winrt::com_ptr<ID3D11VideoDevice>              video_device;
    winrt::com_ptr<ID3D11VideoContext>             video_context;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> vp_enum;
    winrt::com_ptr<ID3D11VideoProcessor>           vp;
    // Single NV12 output texture + output view
    winrt::com_ptr<ID3D11Texture2D>                nv12_tex;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> vp_out_view;

    uint32_t vp_width  = 0;
    uint32_t vp_height = 0;

    // Back-pointer to parent
    CaptureEngine* parent          = nullptr;
    FrameQueue*    queue           = nullptr;
    int64_t        start_100ns     = 0;   // absolute 100ns at session start
    LARGE_INTEGER  qpc_freq        = {};

    // -----------------------------------------------------------------------
    bool setup_video_processor(uint32_t width, uint32_t height) {
        HRESULT hr = d3d_device->QueryInterface(IID_PPV_ARGS(video_device.put()));
        if (FAILED(hr)) { SR_LOG_ERROR(L"QueryInterface(ID3D11VideoDevice) failed: 0x%08X", hr); return false; }

        hr = d3d_context->QueryInterface(IID_PPV_ARGS(video_context.put()));
        if (FAILED(hr)) { SR_LOG_ERROR(L"QueryInterface(ID3D11VideoContext) failed: 0x%08X", hr); return false; }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpcd{};
        vpcd.InputFrameFormat  = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        vpcd.InputWidth        = width;
        vpcd.InputHeight       = height;
        vpcd.OutputWidth       = width;
        vpcd.OutputHeight      = height;
        vpcd.Usage             = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = video_device->CreateVideoProcessorEnumerator(&vpcd, vp_enum.put());
        if (FAILED(hr)) { SR_LOG_ERROR(L"CreateVideoProcessorEnumerator failed: 0x%08X", hr); return false; }

        hr = video_device->CreateVideoProcessor(vp_enum.get(), 0, vp.put());
        if (FAILED(hr)) { SR_LOG_ERROR(L"CreateVideoProcessor failed: 0x%08X", hr); return false; }

        // NV12 output texture
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = width;
        td.Height           = height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

        hr = d3d_device->CreateTexture2D(&td, nullptr, nv12_tex.put());
        if (FAILED(hr)) { SR_LOG_ERROR(L"CreateTexture2D(NV12) failed: 0x%08X", hr); return false; }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

        hr = video_device->CreateVideoProcessorOutputView(
            nv12_tex.get(), vp_enum.get(), &ovd, vp_out_view.put());
        if (FAILED(hr)) { SR_LOG_ERROR(L"CreateVideoProcessorOutputView failed: 0x%08X", hr); return false; }

        vp_width  = width;
        vp_height = height;
        SR_LOG_INFO(L"D3D11 Video Processor ready (%ux%u BGRA->NV12)", width, height);
        return true;
    }

    // -----------------------------------------------------------------------
    bool convert_bgra_to_nv12(ID3D11Texture2D* bgra_tex) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;

        winrt::com_ptr<ID3D11VideoProcessorInputView> in_view;
        HRESULT hr = video_device->CreateVideoProcessorInputView(
            bgra_tex, vp_enum.get(), &ivd, in_view.put());
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"CreateVideoProcessorInputView failed: 0x%08X", hr);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable        = TRUE;
        stream.pInputSurface = in_view.get();

        hr = video_context->VideoProcessorBlt(
            vp.get(), vp_out_view.get(), 0, 1, &stream);
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"VideoProcessorBlt failed: 0x%08X", hr);
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    void on_frame_arrived(wgc::Direct3D11CaptureFramePool const& pool,
                          winrt::Windows::Foundation::IInspectable const&)
    {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;

        auto surface = frame.Surface();
        auto access  = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

        winrt::com_ptr<ID3D11Texture2D> bgra_tex;
        HRESULT hr = access->GetInterface(IID_PPV_ARGS(bgra_tex.put()));
        if (FAILED(hr)) {
            SR_LOG_ERROR(L"GetInterface(ID3D11Texture2D) failed: 0x%08X", hr);
            return;
        }

        if (!convert_bgra_to_nv12(bgra_tex.get())) return;

        // Build RenderFrame
        RenderFrame rf;
        // nv12_tex holds the converted texture; ComPtr Attach/copy with AddRef
        rf.texture = nv12_tex.get();
        rf.width   = vp_width;
        rf.height  = vp_height;

        // PTS: relative to session start in 100ns units
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        int64_t now_100ns = static_cast<int64_t>(
            static_cast<double>(now.QuadPart) * 10'000'000.0 /
            static_cast<double>(qpc_freq.QuadPart));
        rf.pts = now_100ns - start_100ns;
        if (rf.pts < 0) rf.pts = 0;

        parent->frames_captured_.fetch_add(1, std::memory_order_relaxed);

        if (queue && !queue->try_push(std::move(rf))) {
            parent->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// ---------------------------------------------------------------------------
// CaptureEngine methods
// ---------------------------------------------------------------------------

CaptureEngine::CaptureEngine()  = default;
CaptureEngine::~CaptureEngine() { stop(); }

bool CaptureEngine::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                FrameQueue* queue)
{
    // WinRT apartment: multi-threaded (matches COM init in wWinMain)
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (...) {}  // already initialised is fine

    impl_ = std::make_unique<CaptureEngineImpl>();
    impl_->d3d_device  = device;
    impl_->d3d_context = context;
    impl_->queue       = queue;
    impl_->parent      = this;
    QueryPerformanceFrequency(&impl_->qpc_freq);

    // --- Build WinRT IDirect3DDevice wrapper from DXGI device ---
    ComPtr<IDXGIDevice> dxgi_dev;
    device->QueryInterface(IID_PPV_ARGS(&dxgi_dev));

    winrt::com_ptr<::IInspectable> insp;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), insp.put());
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"CreateDirect3D11DeviceFromDXGIDevice failed: 0x%08X", hr);
        return false;
    }
    impl_->winrt_device = insp.as<wdx3::IDirect3DDevice>();

    // --- Primary monitor -> GraphicsCaptureItem ---
    HMONITOR hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    hr = factory->CreateForMonitor(
        hmon,
        winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(impl_->item)
    );
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"WGC CreateForMonitor failed: 0x%08X", hr);
        return false;
    }

    auto size = impl_->item.Size();
    capture_width_  = static_cast<uint32_t>(size.Width);
    capture_height_ = static_cast<uint32_t>(size.Height);
    SR_LOG_INFO(L"WGC item: %ux%u", capture_width_, capture_height_);

    // --- BGRA -> NV12 Video Processor ---
    if (!impl_->setup_video_processor(capture_width_, capture_height_)) {
        return false;
    }

    // --- Free-threaded WGC frame pool ---
    impl_->frame_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->winrt_device,
        wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size
    );

    impl_->session = impl_->frame_pool.CreateCaptureSession(impl_->item);

    // Disable yellow border (Win11 22H2+, non-fatal if unavailable)
    try { impl_->session.IsBorderRequired(false); } catch (...) {}

    // Subscribe to frame-arrived events
    impl_->frame_token = impl_->frame_pool.FrameArrived(
        [this](wgc::Direct3D11CaptureFramePool const& pool,
               winrt::Windows::Foundation::IInspectable const& arg) {
            if (running_.load(std::memory_order_relaxed)) {
                impl_->on_frame_arrived(pool, arg);
            }
        }
    );

    return true;
}

bool CaptureEngine::start() {
    if (!impl_) return false;

    // Record absolute start time for PTS calculation
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    impl_->start_100ns = static_cast<int64_t>(
        static_cast<double>(now.QuadPart) * 10'000'000.0 /
        static_cast<double>(impl_->qpc_freq.QuadPart));

    running_.store(true, std::memory_order_release);
    impl_->session.StartCapture();
    SR_LOG_INFO(L"WGC capture started");
    return true;
}

void CaptureEngine::stop() {
    if (!impl_) return;
    running_.store(false, std::memory_order_release);
    try {
        impl_->frame_pool.FrameArrived(impl_->frame_token);
        impl_->session.Close();
        impl_->frame_pool.Close();
    } catch (...) {}
    impl_.reset();
    SR_LOG_INFO(L"WGC capture stopped");
}

} // namespace sr
