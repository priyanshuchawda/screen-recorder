# 1. Recommended Tech Stack (Clear Decision)

## Primary stack (recommended):
1. Language: `C++20`
2. App model/UI: `Win32 + C++/WinRT` (minimal native desktop UI, single-process app)
3. Screen capture: `Windows Graphics Capture` (`Direct3D11CaptureFramePool`, `GraphicsCaptureSession`)
4. Video encode: `Media Foundation H.264 hardware encoder MFT` with `IMFDXGIDeviceManager` (Intel Quick Sync path)
5. Audio capture: `WASAPI` (mic via `IAudioClient3` + `IAudioCaptureClient`, event-driven shared mode)
6. Container/mux: `MP4` via `IMFSinkWriter`
7. Audio codec: `AAC-LC` (48 kHz)

## Why this stack:
Maximum Windows-native performance, low dependency footprint, direct GPU path, best fit for Intel Iris Xe + Windows 11 APIs.

---

# 2. Justification for Each Choice

## Native Windows vs Electron
1. Native wins on RAM/CPU and startup.
2. Electron adds high baseline memory and extra process overhead.
3. Native integrates directly with D3D11, Media Foundation, WASAPI with less latency and fewer copies.

## C++ vs C# (.NET 8)
1. C++ gives tighter control of COM, D3D textures, encoder surfaces, and threading.
2. C# is faster for UI iteration but introduces interop/GC considerations in sustained real-time media pipelines.
3. For 1080p60 stability on 15W CPU, C++ is safer.

## Media Foundation vs FFmpeg vs DirectShow
1. Media Foundation (recommended): best native integration, hardware transform support, stable on Windows 11, low external deps.
2. FFmpeg: powerful and flexible, but larger dependency and integration/maintenance overhead.
3. DirectShow: legacy stack, less suitable for modern Windows Graphics Capture + modern HW encode flow.

---

# 3. High-Level Architecture Diagram (Text-Based)

```text
[UI Layer (Win32/C++/WinRT)]
        |
        v
[Session Controller + State Machine]
        |
        +-------------------------+
        |                         |
        v                         v
[Video Capture Engine]       [Audio Engine (Mic WASAPI)]
(WGC -> D3D11 Texture)       (PCM packets, event-driven)
        |                         |
        v                         v
[GPU Preprocess]
(BGRA->NV12, scale to target)
        |                         |
        +-----------+-------------+
                    v
        [Timestamp/Sync Manager]
                    |
                    v
      [Encoder + Mux Pipeline]
 (H.264 HW MFT + AAC -> MP4 SinkWriter)
                    |
                    v
            [Storage Manager]
      (temp file, rename on finalize)
```

---

# 4. Component Breakdown

1. `UI Layer`
- Start, Pause, Resume, Stop, Mute toggle.
- Shows elapsed time, current FPS, dropped frames, output path.

2. `Session Controller`
- Central state machine: `Idle -> Recording -> Paused -> Recording -> Stopping -> Idle`.
- Owns lifecycle of all engines and enforces valid transitions.

3. `Capture Engine`
- Uses Windows Graphics Capture full-screen monitor item.
- Receives D3D11 frames and pushes into bounded queue.

4. `GPU Preprocess`
- Converts BGRA to NV12 on GPU.
- Optional scaling to fixed target (e.g., 1920x1080) to handle resolution changes cleanly.

5. `Audio Engine`
- Captures mic in shared mode, event-driven buffers.
- Optional future system-audio loopback toggle.
- Emits PCM blocks with timestamps.

6. `Sync Manager`
- Master clock based on QPC.
- Aligns audio/video PTS, tracks paused duration offset.

7. `Encoder/Mux`
- Video: H.264 hardware encoder MFT (Quick Sync path).
- Audio: AAC encoder MFT.
- Mux to MP4 using `IMFSinkWriter`.

8. `Storage Manager`
- Resolves output folder and unique filename.
- Writes to `.partial.mp4`, renames to `.mp4` on successful stop.
- Periodic disk-space checks and graceful stop if critical.

---

# 5. Encoding Strategy

1. Codec: `H.264` (primary recommendation).
- Best compatibility and low encode cost on Iris Xe.
- HEVC can be optional advanced mode, not default.

2. Container: `MP4`.
- Broad compatibility and user expectation.

3. Presets:
- `1080p30`: 8 Mbps target, keyframe every 2 seconds.
- `1080p60`: 12–16 Mbps target (default 14 Mbps), keyframe every 2 seconds.
- Rate control: VBR or constrained VBR for quality/perf balance.

4. Hardware integration plan:
- D3D11 device + DXGI manager shared with encoder.
- Set MF attributes for hardware transforms and low latency.
- Keep path zero-copy GPU where possible.

5. Fallback strategy:
- Attempt 1: H.264 hardware MFT.
- Attempt 2: H.264 software MFT at same resolution/FPS.
- Attempt 3: auto-degrade to 720p30 software with warning.

---

# 6. Audio Handling Plan

1. Microphone capture:
- WASAPI shared mode (`IAudioClient3`), event-driven.
- 48 kHz AAC pipeline.

2. System audio feasibility:
- Feasible with WASAPI loopback.
- Keep out of initial MVP unless required, to reduce complexity.

3. Sync strategy:
- Video timestamps from capture frame timing.
- Audio timestamps from sample count + session clock anchor.
- Drift correction logic in sync manager.

4. Mute/unmute behavior:
- Keep audio timeline continuous.
- On mute: write silence samples (zeroed PCM) instead of dropping packets.
- On unmute: resume real mic PCM seamlessly, no timeline discontinuity.

---

# 7. Pause/Resume Without Corruption

1. Do not tear down encoder on short pause.
2. On pause:
- Stop submitting video/audio samples.
- Record `pause_start_qpc`.
3. On resume:
- Add pause duration to `paused_accumulator`.
- Rebase outgoing timestamps (`effective_pts = raw_pts - paused_accumulator`).
- Force next video frame as keyframe.
4. Result: continuous MP4 timeline, no A/V desync, no broken file structure.

---

# 8. Performance Optimization Strategy

1. Keep GPU path end-to-end (`WGC texture -> GPU color convert -> HW encode`).
2. Use bounded lock-free queues (small depth) to cap memory and latency.
3. Dedicated worker threads:
- UI thread
- capture thread
- audio capture thread
- encode/mux worker
4. Frame drop policy:
- Drop oldest pending video frame if queue full.
- Never block capture callback.
5. Power-aware behavior:
- On battery, default to 1080p30 preset.
- On AC, allow 60 FPS preset.
6. Memory target:
- Keep active buffering conservative; expected runtime RAM ~150–300 MB, under 500 MB target.

---

# 9. Minimal UI Design Approach

1. Single compact window.
2. Controls: `Start`, `Pause/Resume` toggle, `Stop`, `Mic Mute` toggle.
3. Status fields: recording timer, FPS, dropped frames, output file path.
4. Settings panel (minimal): FPS preset (30/60), bitrate preset, default output folder.
5. No background service; app runs only when user launches it.

---

# 10. Storage Management Strategy

1. Default directory:
- `%USERPROFILE%\Videos\Recordings`
- Resolve via `FOLDERID_Videos`, create `Recordings` if missing.

2. Naming convention:
- `ScreenRec_YYYY-MM-DD_HH-mm-ss.mp4`

3. Conflict handling:
- Append `_001`, `_002`, etc.

4. Crash behavior:
- Write to `*.partial.mp4` during recording.
- On normal stop rename to `.mp4`.
- On next startup detect stale partial files and offer "keep/delete/recover attempt".

5. Disk-space handling:
- Preflight check before start.
- Ongoing checks every few seconds.
- Graceful auto-stop with user notification if free space falls below threshold.

---

# 11. Risk Analysis

1. Encoder init fails on specific driver state.
- Mitigation: fallback chain (HW -> SW -> reduced preset), explicit error telemetry.

2. Resolution change mid-recording.
- Mitigation: normalize to fixed output resolution via GPU scaler.

3. Audio device unplugged/disconnected.
- Mitigation: switch to silence stream and notify user; allow rebind without stopping recording.

4. Thermal throttling/frame drops on 15W CPU.
- Mitigation: battery default 30 FPS, bounded queues, frame-drop policy, optional adaptive bitrate.

5. MP4 finalization risk on abrupt crash.
- Mitigation: partial file handling + startup recovery workflow; optional future segmented recording mode.

---

# 12. Development Roadmap (Phased Plan)

1. Phase 1: Core capture + encode prototype
- Full-screen WGC capture
- H.264 HW encode to MP4
- Start/Stop only
- Validate 1080p30 stability and CPU/GPU usage

2. Phase 2: Audio + sync
- Mic capture (WASAPI)
- AAC encode + A/V mux
- Clock sync and drift handling

3. Phase 3: Pause/Resume + mute/unmute
- State machine completion
- Timestamp rebasing for pause/resume
- Silence injection for mute

4. Phase 4: Robustness + storage
- Output directory rules, naming/conflict resolution
- Disk checks, partial file recovery flow
- Device disconnect handling

5. Phase 5: Performance tuning + QA
- 1080p60 optimization
- Battery vs AC behavior
- Long-session tests, drop-frame telemetry, memory budget validation

---

# 13. Final Recommended Stack Summary (Concise)

Build a `native C++20 Windows desktop recorder` using `Windows Graphics Capture + Direct3D11 + Media Foundation + WASAPI`, with `H.264 (Intel Quick Sync) + AAC in MP4`. Use a `state-machine-driven, multi-threaded, bounded-queue pipeline` with GPU-first processing, timestamp-managed pause/resume, silence-based mute handling, and safe auto-save to `%USERPROFILE%\Videos\Recordings`. This is the best balance of performance, stability, maintainability, and low resource usage for Dell Inspiron 15 3520 on Windows 11.
