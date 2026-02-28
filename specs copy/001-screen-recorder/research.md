# Phase 0: Research & Component Strategy

Even though the architecture was provided directly from requirements, this artifact formalizes the technical directions with rationale and alternatives.

---

## Target System Context

| Property | Value | Impact |
|----------|-------|--------|
| **CPU** | Intel i7-1255U (10C/12T, 15W Alder Lake-U, L2: 6.5MB, L3: 12MB) | Supports Quick Sync, but thermal-constrained under sustained load |
| **GPU** | Intel Iris Xe (2 GB shared VRAM, `libmfxhw64.dll` confirmed) | Quick Sync H.264 encoding verified, D3D11 FL 12.1 |
| **Display** | 1920x1080 native, 1536x864 logical, DPI: 96 (100%) | WGC captures at actual render resolution |
| **Audio** | Cirrus Logic HD + Intel SST | WASAPI shared mode supported, all devices status: OK |
| **Disk** | NTFS ~930 GB, ~182 GB free | Async IO for safety on SATA drives |

---

### 1. Screen Capture Approach

- **Decision**: Windows Graphics Capture (WGC) with `Direct3D11CaptureFramePool`.
- **Rationale**: Best fit for Windows 10/11, integrates directly with D3D11 surfaces without CPU roundtrips. Captures at native pixel resolution (1920x1080) regardless of DPI scaling.
- **Alternatives considered**: Desktop Duplication API (DXGI) — rejected because WGC is more modern and easier to isolate per-monitor natively on Win 11. DXGI requires manual DPI handling and is harder to use with multi-monitor hotplug.
- **DPI Note**: System DPI is 96 (100%). The 1536x864 logical resolution is a Windows display setting, not DPI scaling. WGC captures at effective render resolution.

### 2. Video Encoding

- **Decision**: Media Foundation H.264 hardware encoder MFT (using `IMFDXGIDeviceManager`) with strict configuration (CBR, 2s GOP, low-latency flag, Baseline/Main profile, no B-frames).
- **Rationale**: End-to-end zero-copy GPU pipeline, hardware-accelerated via Intel Quick Sync on Iris Xe. Strict config ensures deterministic encoder latency, avoiding stutter on the 15W TDP CPU.
- **Hardware Detection**: Explicitly enumerate `MFT_ENUM_FLAG_HARDWARE`, checking for NV12 and D3D11 device manager support to avoid accidental software fallbacks.
- **Color Space Conversion**: WGC outputs BGRA. System will use D3D11 Video Processor or a dedicated pixel shader to convert to NV12 entirely on the GPU.
- **Bitrate Strategy**:
  - 1080p30 (battery default): 8 Mbps CBR
  - 1080p60 (AC power): 14 Mbps CBR
- **Alternatives considered**: FFmpeg — rejected due to heavy dependency footprint. DirectShow — legacy, poor integration with WGC.

### 2.5 Frame Pacing & Jitter

- **Strategy**: Insert duplicate frames if the timestamp delta exceeds 1.5x the frame interval. If backpressure is hit (queue full at 5 frames), drop the *newest* frame to maintain low latency.
- **Rationale**: WGC does not guarantee perfectly regular frame delivery. Jitter absorption is critical for maintaining smooth MP4 playback.

### 3. Audio Capture

- **Decision**: WASAPI shared mode, event-driven using `IAudioClient3`.
- **Rationale**: Event-driven capture avoids constant polling, gives deterministic PCM packets. Allows easy synthetic silence injection for mute feature.
- **Device**: Cirrus Logic HD Audio is the primary mic on the Dell Inspiron 15 3520.
- **Pipeline**: Capture at device native rate -> `MFResampler` to 48 kHz if needed -> AAC-LC encode -> MP4 mux.
- **Disconnect handling**: On device invalidation, immediately inject zeroed PCM at correct timestamps. Allow rebind on reconnect.

### 3.5 Operational Optimizations

- **Disk IO**: Async disk writing thread with overlapped writes or IO completion ports to prevent `IMFSinkWriter` from blocking on the disk. Protect space with `GetDiskFreeSpaceEx` polling every 5 seconds.
- **Power/Latency**: Use `SetProcessPriorityClass(ABOVE_NORMAL)`, `SetThreadPriority` for capture/encode threads, and `MMCSS` registration for audio thread to prevent thread starvation under load.
- **Power Mode**: Detect AC vs battery via `GetSystemPowerStatus`. Default to 1080p30 on battery (Balanced power plan) to avoid thermal throttling on the 15W CPU.

### 4. Container / Muxing

- **Decision**: MP4 using `IMFSinkWriter`.
- **Rationale**: Broad compatibility; writing requires an exclusive lock allowing read share to prevent OS indexing services from breaking the timeline.
- **Audio codec**: AAC-LC at 48 kHz, 128 kbps.

### 5. Build System

- **Decision**: CMake 4.1.2 + Ninja 1.13.2 for fast builds, with MSVC 14.50 compiler.
- **Rationale**: Ninja provides fastest incremental builds. CMake supports vcpkg for Google Test. Both are already installed.
- **MSVC Install**: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717`
- **Alternative considered**: MSBuild .vcxproj — rejected for less flexibility with test frameworks and CI pipelines. Pure `build.bat` with manual paths (documented in `path.md`) — viable fallback.
