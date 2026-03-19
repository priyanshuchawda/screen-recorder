# Screen Recorder — Performance Optimization Plan

> **Goal:** Eliminate excessive RAM usage and battery drain while maximizing GPU utilization.
> Acceptable output quality: **480p** or even lower — performance > quality.

---

## 🖥️ System Hardware Profile

| Component | Details |
|-----------|---------|
| **CPU** | Intel Core i7-1255U (12th Gen), 10 cores / 12 threads, ~1.7 GHz base |
| **GPU** | Intel Iris Xe Graphics (96 EUs), 2 GB shared VRAM |
| **RAM** | 15.7 GB total, ~1 GB free (very constrained) |
| **Display** | 1920×1080 @ 60 Hz |
| **OS** | Windows 11 Home, Build 26200, 64-bit |
| **Battery** | Dell laptop, currently at ~20%, Balanced power plan |
| **HW Encoder** | Intel Quick Sync H.264 MFT — **available** ✅ |
| **Power** | BatteryStatus=1 (discharging), AC not connected |

### Key Constraints
- **RAM is critical** — only ~1 GB free. Every MB counts.
- **Battery** — laptop on battery power. Must minimize CPU/GPU power draw.
- **Intel Iris Xe** — has Quick Sync HW encoder, Video Processor for color conversion, and shared VRAM with system RAM.
- **Shared VRAM** — GPU memory comes from system RAM. GPU texture waste = RAM waste.

---

## 🎯 Target Configuration (After Optimization)

| Parameter | Current | Optimized | Rationale |
|-----------|---------|-----------|-----------|
| Capture resolution | 1920×1080 → scaled to 1280×720 | **640×480** (or 854×480) | User accepts 480p; saves 75% GPU memory |
| Output resolution | 1280×720 | **640×480** | Encoder processes 4.6× fewer pixels |
| FPS | 24 (clamped) | **15 fps** on battery, 24 on AC | 15fps is smooth enough for screen recording |
| Bitrate | 6 Mbps (clamped) | **1.5 Mbps** on battery, 4 Mbps on AC | 480p15 needs far less bitrate |
| Encoder | HW-first with SW fallback | **HW-only** (Intel Quick Sync forced) | Zero CPU encoding overhead |
| NV12 texture size | 1280×720 × 1.5 = 1.38 MB | 640×480 × 1.5 = **0.46 MB** | 3× smaller per texture |
| Staging texture | New allocation per frame | **Pre-allocated, reused** | Zero alloc/dealloc churn |
| WGC frame pool | 2 frames | 2 frames (keep) | Minimum for WGC |
| BoundedQueue video | 5 slots | **3 slots** | Less GPU memory held |
| Process priority | ABOVE_NORMAL always | **NORMAL when idle**, ABOVE_NORMAL during recording | OS can throttle when idle |
| Camera overlay | Captures even when idle | **Stopped when not visible** | Eliminates idle camera power draw |
| Encode loop yield | 1ms spin | **Condition variable or 16ms sleep** | Allows CPU C-state entry |
| Power monitoring | Once at start | **Every 30 seconds** | Dynamic throttle on AC↔battery |

### Memory Budget (Optimized)

| Resource | Count | Size Each | Total |
|----------|-------|-----------|-------|
| WGC BGRA frame pool | 2 | 640×480×4 = 1.2 MB | 2.4 MB |
| NV12 output textures (ping-pong) | 2 | 640×480×1.5 = 0.46 MB | 0.92 MB |
| Staging texture (SW fallback only) | 1 | 0.46 MB | 0.46 MB |
| Video queue (RenderFrame refs) | 3 | ~ptr size | negligible |
| Audio queue (PCM packets) | 16 | ~1 KB | 16 KB |
| Audio resampler buffer | 1 | ~4 KB | 4 KB |
| **Total GPU/shared memory** | | | **~4 MB** |

vs. Current: ~15-20 MB GPU/shared memory (1280×720 textures + per-frame staging allocs)

---

## 📋 Implementation Phases

### Phase 1: GPU Memory — Stop the Bleeding (RAM fixes)
> **Priority:** 🔴 CRITICAL — Addresses causes #1, #2, #6 from analysis

| # | Task | File(s) | What Changes |
|---|------|---------|--------------|
| 1.1 | **Pre-allocate staging texture** | `video_encoder.cpp` | Create ONE staging texture in `initialize()`, reuse in `encode_frame()` SW path |
| 1.2 | **Double-buffer NV12 output** | `capture_engine.cpp` | Two `nv12_tex` textures, alternate per frame. Capture writes to A while encoder reads B |
| 1.3 | **Cache VP InputView** | `capture_engine.cpp` | Store `ID3D11VideoProcessorInputView` as member, only recreate on resolution change |
| 1.4 | **Reduce default output to 480p** | `capture_engine.cpp` | Change cap from 1280×720 to 640×480 (or 854×480 for 16:9) |
| 1.5 | **Reduce video queue to 3** | `capture_engine.h` | `BoundedQueue<RenderFrame, 3>` — user said even lower quality is fine |

**Expected RAM savings:** ~10-15 MB (GPU shared memory) + elimination of per-frame allocation churn

---

### Phase 2: CPU & Battery — Stop Spinning (Battery fixes)
> **Priority:** 🟡 HIGH — Addresses causes #5, #7, #8, #10, #13

| # | Task | File(s) | What Changes |
|---|------|---------|--------------|
| 2.1 | **Conditional process priority** | `app/main.cpp`, `session_controller.cpp` | `NORMAL_PRIORITY_CLASS` when idle. `ABOVE_NORMAL` only during `start()`, revert in `stop()` |
| 2.2 | **Smarter encode loop yield** | `session_controller.cpp` | Replace 1ms sleep with `std::this_thread::sleep_for(target_interval / 2)` (~33ms for 15fps) |
| 2.3 | **Add condition_variable to BoundedQueue** | `bounded_queue.h` | Add `notify_one()` on push, `wait_for()` on pop. Encode thread truly sleeps when queue empty |
| 2.4 | **Reduce disk poll to 10s** | `storage_manager.h` | Change 5000ms → 10000ms; also increase sleep interval from 250ms → 1000ms |
| 2.5 | **Dynamic power monitoring** | `session_controller.cpp`, `power_mode.h` | Check `GetSystemPowerStatus()` every 30s in encode_loop; dynamically throttle FPS/bitrate |
| 2.6 | **Battery-aggressive encoder profile** | `power_mode.h` | On battery: cap to **15 fps, 1.5 Mbps, 480p**. On AC: 24 fps, 4 Mbps, 720p |

**Expected battery savings:** ~30-40% power reduction during recording

---

### Phase 3: Camera Overlay — Kill Idle Waste
> **Priority:** 🟡 HIGH — Addresses cause #4

| # | Task | File(s) | What Changes |
|---|------|---------|--------------|
| 3.1 | **Stop camera when overlay hidden** | `camera_overlay.cpp` | When `WM_CLOSE` hides the window, also stop the capture thread |
| 3.2 | **Battery-mode camera throttle** | `camera_overlay.cpp` | On battery: reduce to 10fps capture (100ms interval) |
| 3.3 | **Lazy MF init** | `camera_overlay.cpp` | Don't call `MFStartup()` until first frame is actually needed |
| 3.4 | **Reuse frame buffer** | `camera_overlay.cpp` | Pre-allocate `tight_rgba` once, avoid `resize()` per frame |

**Expected savings:** Camera off when hidden = ~2W power savings + ~2 MB RAM

---

### Phase 4: Audio Pipeline — Small Wins
> **Priority:** 🟢 MEDIUM — Addresses causes #9, #11

| # | Task | File(s) | What Changes |
|---|------|---------|--------------|
| 4.1 | **Reuse resampled buffer** | `audio_engine.cpp` | Make `resampled_buf` a member variable, `.clear()` instead of new alloc |
| 4.2 | **Pool IMFSample/IMFMediaBuffer** | `session_controller.cpp` | Pre-create 2-4 IMFSample+buffer pairs, cycle through them |
| 4.3 | **MMCSS only during recording** | `audio_engine.cpp` | Already correct (in capture_loop), but verify cleanup |

**Expected savings:** ~100 fewer allocations/sec, marginal RAM improvement

---

### Phase 5: Encoder Hardening — GPU-First
> **Priority:** 🟢 MEDIUM — Maximize Intel Quick Sync usage

| # | Task | File(s) | What Changes |
|---|------|---------|--------------|
| 5.1 | **Force GPU encoding path** | `video_encoder.cpp` | With 480p input, Intel Quick Sync should handle easily. Log if falling back to SW |
| 5.2 | **Remove SW fallback staging** | `video_encoder.cpp` | If HW encoder active, skip staging texture entirely (DXGI surface buffer directly) |
| 5.3 | **Reduce GOP for lower latency** | `video_encoder.cpp` | 480p15 → GOP = 1 second (15 frames) instead of 2 seconds |
| 5.4 | **HW encoder error resilience** | `video_encoder.cpp` | Reduce catastrophic-fail threshold from 30 → 10 frames, faster fallback |

---

## 🔧 Detailed Implementation Notes

### 1.1 — Pre-allocate Staging Texture

**Current** (in `encode_frame`, SW path):
```cpp
ComPtr<ID3D11Texture2D> staging;
hr = d3d_device_->CreateTexture2D(&td, nullptr, &staging); // NEW EACH FRAME
```

**Fixed** (add member + init once):
```cpp
// In VideoEncoder class:
ComPtr<ID3D11Texture2D> staging_tex_;  // pre-allocated

// In initialize():
D3D11_TEXTURE2D_DESC td{};
td.Width = out_width_; td.Height = out_height_;
td.Format = DXGI_FORMAT_NV12;
td.Usage = D3D11_USAGE_STAGING;
td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
d3d_device_->CreateTexture2D(&td, nullptr, &staging_tex_);

// In encode_frame():
d3d_context_->CopyResource(staging_tex_.Get(), nv12_texture);
// ... Map staging_tex_ instead of creating new one ...
```

### 1.2 — Double-Buffer NV12

**Current**: Single `nv12_tex` — capture writes while encoder reads.

**Fixed**: Ping-pong buffer:
```cpp
// In CaptureEngineImpl:
winrt::com_ptr<ID3D11Texture2D> nv12_tex_[2];
winrt::com_ptr<ID3D11VideoProcessorOutputView> vp_out_view_[2];
uint32_t write_idx_ = 0;

// In on_frame_arrived:
convert_bgra_to_nv12(bgra_tex.get());  // writes to nv12_tex_[write_idx_]
rf.texture = nv12_tex_[write_idx_].get();
write_idx_ ^= 1;  // swap for next frame
```

### 1.4 — Lower Output Resolution to 480p

**Current**:
```cpp
impl_->out_width_  = (capture_width_ > 1280) ? 1280 : capture_width_;
impl_->out_height_ = (capture_height_ > 720) ? 720 : capture_height_;
```

**Fixed**:
```cpp
impl_->out_width_  = 854;   // 480p widescreen (16:9)
impl_->out_height_ = 480;
```

### 2.2 — Smarter Encode Loop Sleep

**Current**:
```cpp
if (frame_queue_->empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 1ms spin!
}
```

**Fixed**:
```cpp
if (frame_queue_->empty()) {
    // Sleep for half the target frame interval — wakes in time for next frame
    // 15fps → 33ms sleep; 24fps → 21ms sleep
    auto half_interval = std::chrono::milliseconds(
        500 / std::max(1u, encoder_->output_fps()));
    std::this_thread::sleep_for(half_interval);
}
```

### 2.5 — Dynamic Power Monitoring

Add to `encode_loop()`:
```cpp
// Check power every 30 seconds
if (now_ms - last_power_check_ms >= 30000) {
    bool on_ac = PowerModeDetector::is_on_ac_power();
    if (on_ac != last_power_ac_) {
        last_power_ac_ = on_ac;
        // Log the change, adjust frame pacing
        SR_LOG_INFO(L"[Power] Switched to %s", on_ac ? L"AC" : L"Battery");
        pacer_.initialize(on_ac ? 24 : 15);
    }
    last_power_check_ms = now_ms;
}
```

### 2.6 — Battery-Aggressive Profile

**Updated `power_mode.h`**:
```cpp
static EncoderProfile clamp_for_power(const EncoderProfile& requested) {
    if (is_on_ac_power()) {
        EncoderProfile ac = requested;
        ac.fps         = std::min(requested.fps, 24u);
        ac.bitrate_bps = std::min(requested.bitrate_bps, 4'000'000u);
        return ac;
    }
    // Battery: aggressive throttle
    EncoderProfile battery = requested;
    battery.fps         = std::min(requested.fps, 15u);
    battery.bitrate_bps = std::min(requested.bitrate_bps, 1'500'000u);
    battery.width       = std::min(requested.width,  854u);
    battery.height      = std::min(requested.height, 480u);
    return battery;
}
```

---

## 📊 Expected Results

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| GPU shared memory | ~15-20 MB | ~4 MB | **75% less** |
| Texture allocs/sec | ~30 | 0 | **100% eliminated** |
| CPU encode loop wakes/sec | ~1000 | ~30 | **97% fewer wakeups** |
| Camera idle power | ~2W continuous | 0W (off when hidden) | **100% eliminated** |
| Battery FPS | 24 | 15 | **37% fewer frames** |
| Battery bitrate | 6 Mbps | 1.5 Mbps | **75% less encoding** |
| Process priority (idle) | ABOVE_NORMAL | NORMAL | OS can throttle |
| Encode pixel throughput | 1280×720×24 = 22M px/s | 854×480×15 = 6.1M px/s | **72% reduction** |

---

## ✅ Verification Plan

After implementation, verify with:

1. **RAM**: Run the app, start recording for 60s. Check via Task Manager that Working Set stays under **80 MB**.
2. **GPU**: Open Task Manager → Performance → GPU. Shared GPU memory usage should stay under **50 MB**.
3. **CPU**: During recording, CPU usage should be < **5%** (HW encoder) or < **15%** (SW fallback).
4. **Battery**: Record for 5 minutes on battery. Compare power draw via `powercfg /batteryreport`.
5. **Output quality**: Verify 854×480 (or 640×480) MP4 plays correctly in Windows Media Player.

---

## 📁 Files Modified (Summary)

| File | Phase | Changes |
|------|-------|---------|
| `src/capture/capture_engine.h` | 1 | Reduce queue capacity to 3 |
| `src/capture/capture_engine.cpp` | 1 | Double-buffer NV12, cache VP InputView, 480p output |
| `src/encoder/video_encoder.h` | 1 | Add `staging_tex_` member |
| `src/encoder/video_encoder.cpp` | 1, 5 | Pre-alloc staging, GPU encoding priority, smaller GOP |
| `src/encoder/power_mode.h` | 2 | Battery-aggressive profile (15fps, 1.5Mbps, 480p) |
| `src/controller/session_controller.cpp` | 2, 4 | Smarter sleep, dynamic power check, IMFSample pooling |
| `src/app/main.cpp` | 2 | Conditional process priority |
| `src/app/camera_overlay.cpp` | 3 | Stop capture when hidden, battery throttle |
| `src/utils/bounded_queue.h` | 2 | Optional: condition_variable for blocking pop |
| `src/storage/storage_manager.h` | 2 | Reduce poll frequency to 10s |
| `src/audio/audio_engine.cpp` | 4 | Reuse resampled buffer |

---

## ⏱️ Implementation Order

```
Phase 1 (GPU Memory) ──┐
                       ├── Phase 2 (CPU/Battery) ──┐
                       │                           ├── Phase 4 (Audio)
Phase 3 (Camera) ──────┘                           │
                                                   └── Phase 5 (Encoder)
```

Phase 1 and 3 are **independent** and can be done in parallel.
Phase 2 depends on Phase 1 (encode loop changes need new queue).
Phase 4 and 5 are **independent** polish.

> **Estimated total: ~11 files, ~500 lines changed, zero new dependencies.**
