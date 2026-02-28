# Implementation Plan: Full Screen Recorder

**Branch**: `001-screen-recorder` | **Date**: 2026-02-24 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-screen-recorder/spec.md`

---

## Summary

Design and implement a lightweight, high-performance screen recorder application for Windows, primarily focusing on 1080p60 recording with audio capture, built entirely using native Windows APIs (C++20, WGC, Media Foundation, WASAPI).

---

## Target System Profile

| Property | Value |
|----------|-------|
| **Machine** | Dell Inspiron 15 3520 |
| **OS** | Windows 11 Home (Build 26200) |
| **CPU** | Intel i7-1255U (10C/12T, Alder Lake-U, 15W TDP, L2: 6.5MB, L3: 12MB) |
| **RAM** | 16 GB |
| **GPU** | Intel Iris Xe Graphics (2 GB shared VRAM, Quick Sync confirmed via `libmfxhw64.dll`) |
| **Display** | 1920x1080 native, 1536x864 logical, DPI: 96 (100%) |
| **Disk** | ~930 GB NTFS, ~182 GB free |
| **Audio** | Intel SST + Cirrus Logic HD Audio |
| **MSVC** | VS Community 2026 (v18.0), Toolset 14.50.35717, Install: `C:\Program Files\Microsoft Visual Studio\18\Community` |
| **Windows SDK** | 10.0.26100 (all headers/libs verified: d3d11.h, mfapi.h, audioclient.h, WGC, cppwinrt) |
| **Build Tools** | CMake 4.1.2, Ninja 1.13.2, Git 2.51.2 |
| **Media Foundation** | mfplat.dll v10.0.26100.4768, Intel QSV `libmfxhw64.dll` present |

---

## Technical Context

| Property | Value |
|----------|-------|
| **Language** | C++20 |
| **Primary Dependencies** | Win32, C++/WinRT, Windows Graphics Capture, Direct3D11, Media Foundation, WASAPI |
| **Storage** | Local files (`%USERPROFILE%\Videos\Recordings`) |
| **Testing** | Google Test (state machine, sync manager, queue tests) |
| **Target Platform** | Windows 10/11 (64-bit) |
| **Project Type** | Native Desktop Application (single-process) |
| **Build System** | CMake 4.1.2 + Ninja 1.13.2 (fast builds) with MSVC 14.50 compiler |
| **Performance Goals** | 1080p at 60 fps without tearing/drops, zero-copy GPU path, <100ms A/V sync drift over 60 min |
| **Constraints** | Single-process, max 500MB runtime memory, bounded queues (max 5 frames) |

---

## Constitution Check

*GATE: Passed*

- **I. Native Windows Performance**: Confirmed (C++20, D3D11, Media Foundation).
- **II. State Machine & Thread Ownership**: Included Session Controller and dedicated threads.
- **III. Bounded Queues & Backpressure**: Queue limit set to 5 frames, drop policy applied.
- **IV. Encoder Fallback & Downgrade Policy**: Deterministic fallback chain specified.
- **V. Continuous Media Timeline**: Silence injection and pause/resume rebasing specified.
- **VI. Resilient Storage & Crash Recovery**: `.partial.mp4` flow designed.
- **VII. DPI-Aware Capture**: WGC captures actual render resolution; system DPI is 96 (100%), logical 1536x864.
- **VIII. Quick Sync Verified**: `libmfxhw64.dll` confirmed present on system.

---

## Thread Model Ownership

A strictly defined thread ownership model is required to avoid deadlocks:

| Thread | Owns | Priority |
|--------|------|----------|
| **UI Thread** | State machine triggers, window messages | Normal |
| **Capture Thread** | WGC frame pool, GPU preprocess | Above Normal |
| **Audio Thread** | WASAPI capture, silence injection | MMCSS registered |
| **Encode Thread** | MF SinkWriter, H.264/AAC MFTs | Above Normal |
| **IO Thread** | File system, disk space monitoring | Normal |

**Rules**:
- No thread may directly call into another thread's owned resources
- All cross-thread communication goes through bounded lock-free queues
- UI thread never blocks on media pipeline operations

---

## Project Structure

### Documentation (this feature)

```text
specs/001-screen-recorder/
├── spec.md           # Feature specification + requirements
├── plan.md           # Implementation plan (this file)
├── research.md       # Technology decisions + rationale
├── data-model.md     # State machines + data types
├── tasks.md          # Implementation task list
├── quickstart.md     # Build & run instructions
└── checklists/
    └── requirements.md   # Spec quality checklist
```

### Source Code (repository root)

```text
src/
├── app/                  # UI Layer (Win32/C++/WinRT), Main entry point, WGC consent
├── controller/           # Session Controller (State Machine)
├── capture/              # Capture Engine (WGC) & GPU Preprocess (BGRA->NV12)
├── audio/                # Audio Engine (WASAPI capture, silence injection, resampler)
├── encoder/              # Encoder/Mux (Media Foundation H.264/AAC, SinkWriter, fallback)
├── sync/                 # Sync Manager (QPC clock, PTS alignment, frame pacing)
├── storage/              # Storage Manager (Async IO, disk monitoring, partial file handling)
└── utils/                # Bounded queues, threading primitives, QPC wrappers, logging

tests/
├── unit/                 # State machine tests, queue tests, sync logic tests
└── integration/          # Fallback simulation, file lock tests, 60-min stress test

CMakeLists.txt            # Root build configuration
```

**Structure Decision**: Single C++20 project with CMake, organized into module directories matching the architecture diagram. CMake was chosen over MSBuild for better CLI build support, vcpkg integration, and CI compatibility.

---

## Phase X: Verification (MANDATORY)

> This section MUST be completed and all items checked before the project is considered done.

### Build Verification

- [ ] `cmake --build . --config Release` completes without errors
- [ ] `cmake --build . --config Debug` completes without errors
- [ ] No compiler warnings at `/W4` warning level

### Runtime Verification

- [ ] Application launches and is ready to record in < 2 seconds (SC-001)
- [ ] Start/Stop recording produces valid MP4 with audio (US1)
- [ ] Pause/Resume produces continuous timeline, no desync (US2, SC-002)
- [ ] Mute/Unmute produces silence segments accurately (US3)
- [ ] FPS preset change takes effect on next recording (US4)
- [ ] Output directory change takes effect on next recording (US4)
- [ ] Low disk space triggers graceful auto-stop (US5, SC-006)
- [ ] Orphaned .partial.mp4 detected on startup with recovery prompt (US5)

### Performance Verification

- [ ] A/V drift < 100ms over 60-minute recording (SC-003)
- [ ] Frame drops < 5% during 1080p60 under normal conditions (SC-007)
- [ ] Memory does not grow > 5% over 60-minute session (SC-005, SC-008)
- [ ] Memory stays below 500 MB total during sustained recording

### Edge Case Verification

- [ ] Resolution change mid-recording: GPU scaler handles without crash
- [ ] Mic unplug mid-recording: silence injection, no crash
- [ ] DPI scaling (125%): capture is native 1920x1080
- [ ] Hardware encoder unavailable: software fallback chain works
- [ ] Battery power: defaults to 1080p30 preset

### Encoder Fallback Verification

- [ ] Attempt 1: H.264 Hardware MFT initializes on Iris Xe
- [ ] Attempt 2: Software MFT fallback works when HW is blocked
- [ ] Attempt 3: 720p30 degraded mode works as last resort

---

## Complexity Tracking

*(No constitution violations, therefore no justification required)*
