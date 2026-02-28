# Data Model: State Machines & Data Types

This application is strictly state-machine and pipeline oriented. All data flows through bounded queues between dedicated threads.

---

## Key State Machines

### 1. SessionMachine

```text
                    Start
          ┌────────────────────────┐
          │                        ▼
       [Idle] ◄──────────── [Stopping]
                                 ▲
                                 │ Stop / Low Disk
                                 │
       [Recording] ◄────► [Paused]
          │    Pause     Resume   │
          │                      │
          └──────────────────────┘
                 (both can Stop)
```

- **States**: `Idle`, `Recording`, `Paused`, `Stopping`
- **Transitions**:
  - `Idle` -> `Recording` (Start request)
  - `Recording` -> `Paused` (Pause request)
  - `Paused` -> `Recording` (Resume request)
  - `Recording` -> `Stopping` (Stop request, low disk space)
  - `Paused` -> `Stopping` (Stop request, low disk space)
  - `Stopping` -> `Idle` (On finalized flush)
- **Invalid transitions** (rejected by state machine):
  - `Idle` -> `Paused` (cannot pause without recording)
  - `Idle` -> `Stopping` (nothing to stop)
  - `Stopping` -> `Recording` (must complete finalization first)

### 2. AudioMuteState (sub-state within Recording/Paused)

- **States**: `Live`, `Muted`
- **Behavior**:
  - `Live`: WASAPI PCM data flows to encoder
  - `Muted`: Zeroed PCM (silence) flows to encoder at correct timestamps
  - Transition between states is instant, no timeline gap

---

## Key Data Types

### 1. RenderFrame

| Field | Type | Notes |
|-------|------|-------|
| `texture` | `ComPtr<ID3D11Texture2D>` | GPU-backed, NV12 format (after conversion) |
| `pts` | `int64_t` | QPC ticks mapped to 100ns units |
| `width` | `uint32_t` | Frame width in pixels |
| `height` | `uint32_t` | Frame height in pixels |
| `is_duplicate` | `bool` | True if inserted by frame pacer |

**Lifecycle**: Created by CaptureEngine, converted to NV12 by GPU preprocess, consumed by VideoEncoder. Move-only semantics (no copies).

### 2. AudioPacket

| Field | Type | Notes |
|-------|------|-------|
| `buffer` | `std::vector<uint8_t>` | Raw PCM data (or zeroed silence block) |
| `frame_count` | `uint32_t` | Number of audio frames in buffer |
| `pts` | `int64_t` | Derived from sample count + session clock anchor |
| `is_silence` | `bool` | True if synthetic silence (muted) |
| `sample_rate` | `uint32_t` | Sample rate (e.g., 48000 Hz) |
| `channels` | `uint16_t` | Channel count (typically 1 or 2) |

**Lifecycle**: Created by AudioEngine (or silence injector), consumed by AAC encoder via SinkWriter. Copied (small buffers, ~4-20 KB per packet).

### 3. FileContext

| Field | Type | Notes |
|-------|------|-------|
| `active_path` | `std::wstring` | e.g., `%USERPROFILE%\Videos\Recordings\ScreenRec_2026-02-24_13-37-16.partial.mp4` |
| `final_path` | `std::wstring` | e.g., `%USERPROFILE%\Videos\Recordings\ScreenRec_2026-02-24_13-37-16.mp4` |
| `file_handle` | `HANDLE` | Exclusive write, shared read (FILE_SHARE_READ) |
| `bytes_written` | `uint64_t` | Running total for disk-space monitoring |

**Lifecycle**: Created by StorageManager on Start, owned exclusively by IO thread, finalized on Stop (rename partial -> final).

### 4. EncoderProfile

| Field | Type | Notes |
|-------|------|-------|
| `fps` | `uint32_t` | 30 or 60 |
| `bitrate_bps` | `uint32_t` | 8,000,000 (30fps) or 14,000,000 (60fps) |
| `width` | `uint32_t` | 1920 |
| `height` | `uint32_t` | 1080 |
| `gop_seconds` | `uint32_t` | 2 |
| `rate_control` | `enum` | CBR |
| `profile` | `enum` | Baseline or Main |
| `low_latency` | `bool` | true |
| `b_frames` | `uint32_t` | 0 |

---

## Queue Specifications

| Queue | Type | Max Depth | Drop Policy | Producer | Consumer |
|-------|------|-----------|-------------|----------|----------|
| Video Queue | `BoundedQueue<RenderFrame>` | 5 frames | Drop newest | Capture Thread | Encode Thread |
| Audio Queue | `BoundedQueue<AudioPacket>` | 10 packets | Never block capture; drop oldest if full | Audio Thread | Encode Thread |

**Rules**:
- Producer MUST use `try_push()` (non-blocking). If full, apply drop policy
- Consumer uses `wait_pop()` with timeout
- Queue is lock-free (SPSC or MPSC ring buffer)
