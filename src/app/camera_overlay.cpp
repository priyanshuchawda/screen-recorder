#include "app/camera_overlay.h"
#include "utils/logging.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sr {

using Microsoft::WRL::ComPtr;

namespace {

struct CameraFormatChoice {
    ComPtr<IMFMediaType> native_type;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fps_num = 30;
    UINT32 fps_den = 1;
    LONGLONG score = LLONG_MIN;
};

LONGLONG score_format(UINT32 w, UINT32 h, UINT32 fps_num, UINT32 fps_den, bool on_battery) {
    const UINT32 max_w = on_battery ? 1280 : 1920;
    const UINT32 max_h = on_battery ? 720 : 1080;
    const UINT32 pref_w = on_battery ? 1280 : 1920;
    const UINT32 pref_h = on_battery ? 720 : 1080;

    const LONGLONG pixels = static_cast<LONGLONG>(w) * static_cast<LONGLONG>(h);
    const LONGLONG max_pixels = static_cast<LONGLONG>(max_w) * static_cast<LONGLONG>(max_h);
    const LONGLONG pref_pixels = static_cast<LONGLONG>(pref_w) * static_cast<LONGLONG>(pref_h);

    LONGLONG score = 0;
    if (pixels <= max_pixels) {
        score += pixels;
    } else {
        score += max_pixels - ((pixels - max_pixels) * 2);
    }

    score -= llabs(pref_pixels - pixels) / 8;

    const double fps = fps_den ? static_cast<double>(fps_num) / static_cast<double>(fps_den) : 30.0;
    const double fps_clamped = fps > 30.0 ? 30.0 : fps;
    score += static_cast<LONGLONG>(fps_clamped * 50000.0);

    const double aspect = h ? static_cast<double>(w) / static_cast<double>(h) : (16.0 / 9.0);
    const double aspect_diff = std::abs(aspect - (16.0 / 9.0));
    score -= static_cast<LONGLONG>(aspect_diff * 150000.0);
    return score;
}

bool choose_camera_format(IMFSourceReader* reader,
                          bool on_battery,
                          UINT32& chosen_w,
                          UINT32& chosen_h,
                          UINT32& chosen_fps_num,
                          UINT32& chosen_fps_den) {
    CameraFormatChoice best;

    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> native_type;
        HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &native_type);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || !native_type) {
            continue;
        }

        UINT32 w = 0;
        UINT32 h = 0;
        if (FAILED(MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
            continue;
        }

        UINT32 fps_num = 30;
        UINT32 fps_den = 1;
        if (FAILED(MFGetAttributeRatio(native_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) || fps_den == 0) {
            fps_num = 30;
            fps_den = 1;
        }

        LONGLONG score = score_format(w, h, fps_num, fps_den, on_battery);
        if (score > best.score) {
            best.native_type = native_type;
            best.width = w;
            best.height = h;
            best.fps_num = fps_num;
            best.fps_den = fps_den;
            best.score = score;
        }
    }

    if (!best.native_type) {
        return false;
    }

    chosen_w = best.width;
    chosen_h = best.height;
    chosen_fps_num = best.fps_num;
    chosen_fps_den = best.fps_den;

    ComPtr<IMFMediaType> rgb_type;
    HRESULT hr = MFCreateMediaType(&rgb_type);
    if (FAILED(hr) || !rgb_type) {
        return false;
    }

    rgb_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(rgb_type.Get(), MF_MT_FRAME_SIZE, chosen_w, chosen_h);
    MFSetAttributeRatio(rgb_type.Get(), MF_MT_FRAME_RATE, chosen_fps_num, chosen_fps_den);
    MFSetAttributeRatio(rgb_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    rgb_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb_type.Get());
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

} // namespace

bool CameraOverlay::detect_on_battery() const {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return false;
    return s.ACLineStatus == 0;
}

void CameraOverlay::resize_capture_to_client() {
    if (host_hwnd_) {
        InvalidateRect(host_hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK CameraOverlay::HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<CameraOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* ptr = reinterpret_cast<CameraOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ptr));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (self) self->resize_capture_to_client();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old_obj = SelectObject(mem_dc, bmp);

            if (self) {
                RECT mem_rc{ 0, 0, w, h };
                self->draw_latest_frame(mem_dc, mem_rc);
            }

            BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

            SelectObject(mem_dc, old_obj);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void CameraOverlay::apply_preview_tuning() {
    capture_interval_ms_.store(on_battery_ ? 80 : 33, std::memory_order_relaxed);
}

void CameraOverlay::draw_latest_frame(HDC hdc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (latest_frame_.empty() || frame_width_ == 0 || frame_height_ == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(210, 210, 210));
        RECT txt = rc;
        DrawTextW(hdc, L"No camera frame", -1, &txt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(frame_width_);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(frame_height_); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const int dst_w = rc.right - rc.left;
    const int dst_h = rc.bottom - rc.top;
    const double src_ar = frame_height_ ? (static_cast<double>(frame_width_) / static_cast<double>(frame_height_)) : 1.0;
    const double dst_ar = dst_h > 0 ? (static_cast<double>(dst_w) / static_cast<double>(dst_h)) : src_ar;

    int draw_w = dst_w;
    int draw_h = dst_h;
    int draw_x = 0;
    int draw_y = 0;
    if (src_ar > dst_ar) {
        draw_h = static_cast<int>(dst_w / src_ar);
        draw_y = (dst_h - draw_h) / 2;
    } else {
        draw_w = static_cast<int>(dst_h * src_ar);
        draw_x = (dst_w - draw_w) / 2;
    }

    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(
        hdc,
        draw_x, draw_y, draw_w, draw_h,
        0, 0, static_cast<int>(frame_width_), static_cast<int>(frame_height_),
        latest_frame_.data(),
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY);
}

void CameraOverlay::capture_loop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coinit_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"CameraOverlay: MFStartup failed: 0x%08X", hr);
        if (coinit_ok) CoUninitialize();
        return;
    }

    ComPtr<IMFAttributes> enum_attrs;
    hr = MFCreateAttributes(&enum_attrs, 1);
    if (FAILED(hr)) {
        SR_LOG_ERROR(L"CameraOverlay: MFCreateAttributes(enum) failed: 0x%08X", hr);
        MFShutdown();
        if (coinit_ok) CoUninitialize();
        return;
    }

    enum_attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 dev_count = 0;
    hr = MFEnumDeviceSources(enum_attrs.Get(), &devices, &dev_count);
    if (FAILED(hr) || dev_count == 0) {
        SR_LOG_WARN(L"CameraOverlay: no video capture devices found");
        if (devices) CoTaskMemFree(devices);
        MFShutdown();
        if (coinit_ok) CoUninitialize();
        return;
    }

    ComPtr<IMFMediaSource> source;
    hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
    WCHAR dev_name[256]{};
    devices[0]->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, dev_name, _countof(dev_name), nullptr);
    SR_LOG_INFO(L"CameraOverlay: using MF camera device: %s", dev_name);

    for (UINT32 i = 0; i < dev_count; ++i) {
        if (devices[i]) devices[i]->Release();
    }
    CoTaskMemFree(devices);

    if (FAILED(hr) || !source) {
        SR_LOG_ERROR(L"CameraOverlay: ActivateObject camera failed: 0x%08X", hr);
        MFShutdown();
        if (coinit_ok) CoUninitialize();
        return;
    }

    ComPtr<IMFAttributes> reader_attrs;
    MFCreateAttributes(&reader_attrs, 1);
    reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), reader_attrs.Get(), &reader);
    if (FAILED(hr) || !reader) {
        SR_LOG_ERROR(L"CameraOverlay: MFCreateSourceReaderFromMediaSource failed: 0x%08X", hr);
        MFShutdown();
        if (coinit_ok) CoUninitialize();
        return;
    }

    UINT32 chosen_w = 0;
    UINT32 chosen_h = 0;
    UINT32 chosen_fps_num = 30;
    UINT32 chosen_fps_den = 1;
    if (!choose_camera_format(reader.Get(), on_battery_, chosen_w, chosen_h, chosen_fps_num, chosen_fps_den)) {
        SR_LOG_WARN(L"CameraOverlay: failed to set preferred RGB32 camera format, using reader default");
    } else {
        const double chosen_fps = static_cast<double>(chosen_fps_num) / static_cast<double>(chosen_fps_den ? chosen_fps_den : 1);
        SR_LOG_INFO(L"CameraOverlay: preview format %ux%u @ %.2f fps (RGB32)", chosen_w, chosen_h, chosen_fps);
    }

    capture_running_.store(true, std::memory_order_release);
    while (capture_running_.load(std::memory_order_acquire)) {
        DWORD stream_index = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                &stream_index, &flags, &ts, &sample);
        if (FAILED(hr)) {
            SR_LOG_WARN(L"CameraOverlay: ReadSample failed: 0x%08X", hr);
            Sleep(80);
            continue;
        }

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            Sleep(10);
            continue;
        }

        if (sample) {
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* data = nullptr;
                DWORD max_len = 0, cur_len = 0;
                if (SUCCEEDED(buf->Lock(&data, &max_len, &cur_len)) && data && cur_len > 0) {
                    ComPtr<IMFMediaType> current_type;
                    UINT32 w = frame_width_, h = frame_height_;
                    if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current_type)) && current_type) {
                        MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
                    }

                    std::vector<uint8_t> tight_rgba;
                    const size_t tight_size = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
                    bool copied = false;

                    ComPtr<IMF2DBuffer> buf2d;
                    if (SUCCEEDED(buf.As(&buf2d)) && buf2d) {
                        BYTE* scan0 = nullptr;
                        LONG stride = 0;
                        if (SUCCEEDED(buf2d->Lock2D(&scan0, &stride)) && scan0 && tight_size > 0) {
                            tight_rgba.resize(tight_size);
                            const size_t row_bytes = static_cast<size_t>(w) * 4;
                            for (UINT32 row = 0; row < h; ++row) {
                                const BYTE* src_row = scan0 + static_cast<ptrdiff_t>(row) * static_cast<ptrdiff_t>(stride);
                                std::memcpy(tight_rgba.data() + static_cast<size_t>(row) * row_bytes, src_row, row_bytes);
                            }
                            buf2d->Unlock2D();
                            copied = true;
                        }
                    }

                    if (!copied) {
                        if (tight_size > 0 && cur_len >= tight_size) {
                            tight_rgba.assign(data, data + tight_size);
                        } else {
                            tight_rgba.assign(data, data + cur_len);
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        latest_frame_.swap(tight_rgba);
                        frame_width_ = w;
                        frame_height_ = h;
                    }

                    buf->Unlock();
                    if (host_hwnd_) InvalidateRect(host_hwnd_, nullptr, FALSE);
                }
            }
        }

        Sleep(capture_interval_ms_.load(std::memory_order_relaxed));
    }

    source->Shutdown();
    MFShutdown();
    if (coinit_ok) CoUninitialize();
}

bool CameraOverlay::start(HWND owner) {
    if (running_) return true;

    constexpr int host_w = 360;
    constexpr int host_h = 220;

    int x = 40;
    int y = 40;
    if (owner) {
        RECT rc{};
        if (GetWindowRect(owner, &rc)) {
            x = rc.right + 12;
            y = rc.top;
        }
    }

    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"SRCameraOverlayHost";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    owner_ = owner;
    host_hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SRCameraOverlayHost",
        L"Camera Preview",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, host_w, host_h,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!host_hwnd_) {
        SR_LOG_ERROR(L"CameraOverlay: host window create failed: %u", GetLastError());
        return false;
    }

    SetWindowPos(host_hwnd_, HWND_TOPMOST, x, y, host_w, host_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    resize_capture_to_client();

    on_battery_ = detect_on_battery();
    apply_preview_tuning();
    capture_thread_ = std::thread(&CameraOverlay::capture_loop, this);

    running_ = true;
    SR_LOG_INFO(L"CameraOverlay: started (%s power)", on_battery_ ? L"battery" : L"AC");
    return true;
}

void CameraOverlay::refresh_power_profile() {
    if (!running_) return;

    bool now_battery = detect_on_battery();
    if (now_battery != on_battery_) {
        on_battery_ = now_battery;
        apply_preview_tuning();
        SR_LOG_INFO(L"CameraOverlay: power mode changed -> %s", on_battery_ ? L"battery" : L"AC");
    }
}

void CameraOverlay::stop() {
    capture_running_.store(false, std::memory_order_release);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (host_hwnd_) {
        DestroyWindow(host_hwnd_);
        host_hwnd_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_frame_.clear();
        frame_width_ = 0;
        frame_height_ = 0;
    }

    if (running_) {
        SR_LOG_INFO(L"CameraOverlay: stopped");
    }
    running_ = false;
}

} // namespace sr
