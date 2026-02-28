---
description: "Task list for feature implementation: Full Screen Recorder"
---

# Tasks: Full Screen Recorder

**Input**: Design documents from `/specs/001-screen-recorder/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md
**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.
**Developer**: Single-developer project — no agent routing required. All tasks are self-assigned.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [X] T001 — Create native C++20 project structure per implementation plan
  - INPUT: `plan.md` project structure diagram
  - OUTPUT: Directory tree created: `src/{app,controller,capture,audio,encoder,sync,storage,utils}`, `tests/{unit,integration}`
  - VERIFY: All directories exist, root `CMakeLists.txt` created

- [X] T002 — Initialize CMake configuration linking WinRT, D3D11, WASAPI, and Media Foundation
  - INPUT: Empty `CMakeLists.txt`, Windows SDK 10.0.26100
  - OUTPUT: CMake builds a minimal Win32 "hello world" window with all libs linked
  - VERIFY: `cmake --build . --config Debug` succeeds, `.exe` runs and shows a window

- [X] T003 [P] — Configure Google Test framework in `tests/` directory
  - INPUT: CMake project from T002
  - OUTPUT: `tests/CMakeLists.txt` with GTest fetched via FetchContent, a sample test passes
  - VERIFY: `ctest --test-dir build` runs and reports 1 passing test

- [X] T004 [P] — Setup basic logging and QPC timing wrappers in `src/utils/`
  - INPUT: None (self-contained utility)
  - OUTPUT: `src/utils/logging.h`, `src/utils/qpc_clock.h` with QPC timestamp helpers
  - VERIFY: Unit test confirms QPC timestamps are monotonic and nanosecond-resolution

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 — Implement thread ownership primitives and bounded lock-free queue (max 5 frames) in `src/utils/`
  - INPUT: `data-model.md` type definitions
  - OUTPUT: `BoundedQueue<T>` template with try_push/try_pop, `ThreadOwner` RAII wrapper
  - VERIFY: Multi-threaded unit test: 2 producers + 1 consumer, queue never exceeds 5 items, no data races (run under TSAN or similar)

- [X] T006 [P] — Implement `StorageManager` scaffolding and directory resolution in `src/storage/`
  - INPUT: `FOLDERID_Videos` shell API
  - OUTPUT: `StorageManager` resolves `%USERPROFILE%\Videos\Recordings`, creates folder if missing, generates unique filenames (`ScreenRec_YYYY-MM-DD_HH-mm-ss.mp4`) with `_001` conflict suffix
  - VERIFY: Unit test creates temp dir, generates 3 filenames, verifies uniqueness and format

- [X] T007 — Implement `SessionMachine` state transitions in `src/controller/`
  - INPUT: State diagram from `data-model.md`
  - OUTPUT: `SessionMachine` class: `Idle -> Recording -> Paused -> Recording -> Stopping -> Idle`, rejects invalid transitions, emits state-change events
  - VERIFY: Unit tests cover all valid transitions AND all invalid transition rejections (at least 10 test cases)

- [X] T008 — Setup D3D11 device and HW Encoder enumeration in `src/encoder/`
  - INPUT: Windows SDK, D3D11 headers
  - OUTPUT: `EncoderProbe` class that creates D3D11 device, enumerates `MFT_ENUM_FLAG_HARDWARE`, checks NV12 and DXGI device manager support
  - VERIFY: Run on target machine (Iris Xe) — logs "HW encoder found: Intel Quick Sync" or appropriate fallback

- [X] T009 — Create `RenderFrame` and `AudioPacket` standard structs in `src/utils/`
  - INPUT: `data-model.md` type definitions
  - OUTPUT: `RenderFrame` (ID3D11Texture2D, int64_t pts), `AudioPacket` (PCM buffer, size, int64_t pts, bool is_silence), `FileContext` (paths, lock handle)
  - VERIFY: Structs compile, sizeof checks documented, move semantics work correctly

**Checkpoint**: Foundation ready — user story implementation can now begin

---

## Phase 3: User Story 1 — Basic Screen and Microphone Recording (Priority: P1) MVP

**Goal**: Users can quickly start and stop a screen recording with microphone audio.

**Independent Test**: Launch the app, start recording, speak into mic, stop recording, verify MP4 plays with audio.

### Implementation for User Story 1

- [ ] T010 [P] [US1] — Implement `CaptureEngine` using WGC for fullscreen in `src/capture/`
  - INPUT: D3D11 device from T008, primary monitor GraphicsCaptureItem
  - OUTPUT: `CaptureEngine` class that acquires WGC permission, creates frame pool, pushes `RenderFrame` into `BoundedQueue`
  - VERIFY: Standalone test captures 30 frames at 30fps target, frames are valid D3D11 textures with monotonic timestamps

- [ ] T011 [P] [US1] — Implement GPU BGRA->NV12 color conversion in `src/capture/`
  - INPUT: `RenderFrame` with BGRA texture from WGC
  - OUTPUT: GPU-side conversion (D3D11 Video Processor or pixel shader) producing NV12 texture in-place
  - VERIFY: Capture 1 frame, convert, read back to CPU, verify NV12 plane layout (Y plane + interleaved UV plane)

- [ ] T012 [P] [US1] — Implement WASAPI shared event-driven mic capture in `src/audio/`
  - INPUT: Default audio capture device
  - OUTPUT: `AudioEngine` class that captures PCM at device native rate, pushes `AudioPacket` to queue, event-driven (no polling)
  - VERIFY: Capture 2 seconds of audio, write to WAV, verify playback matches

- [ ] T013 [US1] — Implement Media Foundation H.264 Encoder with fallback in `src/encoder/`
  - INPUT: NV12 textures from T011, encoder probe from T008
  - OUTPUT: `VideoEncoder` class: CBR, 2s GOP, low-latency, no B-frames, Baseline/Main profile. Implements 3-step fallback chain (TC-009)
  - VERIFY: Encode 60 frames to raw H.264 bitstream, verify with `ffprobe` or `MFCreateSourceReaderFromByteStream`

- [ ] T014 [US1] — Implement `SyncManager` for A/V PTS alignment in `src/sync/`
  - INPUT: QPC clock from T004, video frame timestamps, audio sample timestamps
  - OUTPUT: `SyncManager` class: master clock, aligns video/audio PTS, tracks paused duration offset, frame pacing (duplicate if delta > 1.5x interval, drop newest if queue full)
  - VERIFY: Unit test with synthetic timestamps, assert output PTS sequence is monotonic and correctly paced

- [ ] T015 [US1] — Implement MP4 SinkWriter with async IO in `src/storage/`
  - INPUT: Encoded H.264 + AAC samples
  - OUTPUT: `MuxWriter` class wrapping IMFSinkWriter, writes to `.partial.mp4`, async IO thread with overlapped writes
  - VERIFY: Mux 5 seconds of test video+audio, verify file plays in Windows Media Player / VLC

- [ ] T016 [US1] — Wire engines together via `SessionController` in `src/controller/`
  - INPUT: All engines from T010-T015, state machine from T007
  - OUTPUT: `SessionController` class: Start initializes all engines, runs capture->preprocess->encode->mux pipeline, Stop finalizes and renames file
  - VERIFY: Full pipeline test: record 10s -> stop -> verify MP4 file exists, plays, has audio

- [ ] T017 [US1] — Construct minimal Win32/C++ UI window in `src/app/`
  - INPUT: `SessionController` from T016
  - OUTPUT: Compact Win32 window with Start/Stop buttons, elapsed time display, FPS counter, dropped frame counter, output path display
  - VERIFY: Manual test: buttons trigger state transitions, timer updates, file is saved

**Checkpoint**: User Story 1 fully functional and testable independently (MVP)

---

## Phase 4: User Story 2 — Pause and Resume Recording (Priority: P2)

**Goal**: Users can pause and resume without multiple files or A/V desync.

**Independent Test**: Start recording, pause, wait 5 seconds, resume, stop — verify continuous playback.

### Implementation for User Story 2

- [ ] T018 [P] [US2] — Update `SessionMachine` for `Recording -> Paused -> Recording` in `src/controller/`
  - INPUT: State machine from T007
  - OUTPUT: Pause/Resume transitions added, `pause_start_qpc` captured on pause
  - VERIFY: Unit tests: Idle->Recording->Paused->Recording->Stopping->Idle valid, Idle->Paused rejected

- [ ] T019 [US2] — Implement pause timestamp rebasing in `src/sync/`
  - INPUT: `SyncManager` from T014
  - OUTPUT: `paused_accumulator` tracks total pause duration, output PTS = raw PTS - paused_accumulator. Force keyframe on resume
  - VERIFY: Unit test with simulated 5s pause, verify output PTS jumps correctly without gap

- [ ] T020 [US2] — Ensure Encoder/SinkWriter preserved during pause in `src/encoder/`
  - INPUT: `VideoEncoder` + `MuxWriter` from T013/T015
  - OUTPUT: Pause stops sample submission but does NOT call `Finalize()` or release MFTs. Resume resumes submission
  - VERIFY: Pause 3s, resume, encode 30 more frames — final file plays correctly

- [ ] T021 [US2] — Update UI with Pause/Resume toggle in `src/app/`
  - INPUT: UI from T017
  - OUTPUT: Pause/Resume toggle button, timer pauses during pause, visual indicator (pulsing "Paused" label)
  - VERIFY: Manual test: button toggles state, timer freezes/resumes

**Checkpoint**: User Stories 1 AND 2 both work independently

---

## Phase 5: User Story 3 — Mute and Unmute Microphone (Priority: P2)

**Goal**: Users can silence their mic during recording without stopping capture.

**Independent Test**: Record, mute, speak, unmute, speak — verify silence in muted portion.

### Implementation for User Story 3

- [ ] T022 [P] [US3] — Implement silence injection in `src/audio/`
  - INPUT: `AudioEngine` from T012
  - OUTPUT: `injectSilence()` method: pushes zeroed PCM `AudioPacket` with correct timestamp continuity when muted
  - VERIFY: Unit test: 1s real audio -> 1s silence -> 1s real audio, verify PCM buffer contents

- [ ] T023 [US3] — Wire Mute state through `SessionController` in `src/controller/`
  - INPUT: `SessionController` from T016, `AudioEngine` from T012
  - OUTPUT: Mute flag in controller, toggles between WASAPI capture and silence injection. Audio timeline remains continuous
  - VERIFY: Integration test: record 5s with mute at 2-3s, verify final MP4 has silence in correct segment

- [ ] T024 [US3] — Update UI with Mic Mute/Unmute toggle in `src/app/`
  - INPUT: UI from T021
  - OUTPUT: Mic mute toggle button with visual indicator (e.g., mic icon with strikethrough), state reflected in real-time
  - VERIFY: Manual test: toggle works, visual feedback immediate

**Checkpoint**: Core recording flows (Start/Stop/Pause/Mute) entirely functional

---

## Phase 6: User Story 4 — Configuration Settings (Priority: P3)

**Goal**: Users can adjust FPS and output directory.

**Independent Test**: Change FPS to 60 and change output folder, record, verify file properties match.

### Implementation for User Story 4

- [ ] T025 [P] [US4] — Update `StorageManager` for dynamic directory overrides in `src/storage/`
  - INPUT: `StorageManager` from T006
  - OUTPUT: `setOutputDirectory()` method, validates path exists (or creates it), persists preference
  - VERIFY: Unit test: set custom dir, generate filename, verify path is correct

- [ ] T026 [P] [US4] — Abstract encoder profile for dynamic FPS presets in `src/encoder/`
  - INPUT: `VideoEncoder` from T013
  - OUTPUT: `EncoderProfile` struct: {fps: 30|60, bitrate: 8|14 Mbps, resolution: 1080p}. Encoder accepts profile at init
  - VERIFY: Create encoder with 60fps profile, verify MFT attributes match

- [ ] T027 [US4] — Implement Settings Panel UI in `src/app/`
  - INPUT: UI from T024
  - OUTPUT: Settings overlay/dialog: FPS dropdown (30/60), output directory selector (folder browser dialog), settings persisted to JSON/INI
  - VERIFY: Manual test: change settings, restart app, settings restored

---

## Phase 7: User Story 5 — Fault Tolerance and Crash Recovery (Priority: P3)

**Goal**: Long recordings are protected against crashes and disk-full conditions.

**Independent Test**: Simulate low disk -> verify graceful stop. Force-kill process -> verify recovery prompt.

### Implementation for User Story 5

- [ ] T028 [P] [US5] — Implement async disk space polling (`GetDiskFreeSpaceEx`) in `src/storage/`
  - INPUT: `StorageManager` from T025
  - OUTPUT: Background check every 5 seconds, fires callback when free space < 500 MB
  - VERIFY: Unit test with mock: simulate low space, verify callback fires within 10s

- [ ] T029 [P] [US5] — Implement `.partial.mp4` with exclusive write lock in `src/storage/`
  - INPUT: `MuxWriter` from T015
  - OUTPUT: CreateFile with GENERIC_WRITE + FILE_SHARE_READ. Rename to final `.mp4` on successful stop
  - VERIFY: While recording, open partial file in another process for reading — succeeds. Attempt write — fails (locked)

- [ ] T030 [US5] — Add startup orphan detection and recovery UI in `src/app/`
  - INPUT: `StorageManager` from T028
  - OUTPUT: On startup, scan for `*.partial.mp4` in output dir. Show dialog: "Found incomplete recording. Recover / Delete / Ignore"
  - VERIFY: Create a dummy .partial.mp4, launch app, verify dialog appears

- [ ] T031 [US5] — Implement graceful auto-finalize on low disk in `src/controller/`
  - INPUT: `SessionController` from T016, disk space callback from T028
  - OUTPUT: On low-disk callback, trigger `Stopping` state, finalize file, show user notification
  - VERIFY: Integration test: fill temp partition to near-full, start recording, verify auto-stop and notification

---

## Phase 8: Polish & Cross-Cutting Constraints (Architecture Rules)

**Purpose**: Addressing specific edge cases and mandatory technical constraints from the specification.

- [ ] T032 [P] — Implement `MFResampler` for audio sample rate mismatches in `src/audio/`
  - INPUT: `AudioEngine` from T012
  - OUTPUT: Resampler converts device native rate to 48 kHz AAC pipeline rate. Handles device invalidation callbacks
  - VERIFY: Test with device at 44.1 kHz, verify resampled output is 48 kHz without drift

- [ ] T033 [P] — Implement thread/process priority optimizations in `src/app/`
  - INPUT: Thread model from plan.md
  - OUTPUT: `SetProcessPriorityClass(ABOVE_NORMAL)`, `SetThreadPriority` for capture/encode threads, `MMCSS` registration for audio thread
  - VERIFY: Verify via Task Manager / Process Explorer that priorities are set correctly

- [ ] T034 [P] — Implement dynamic resolution change handling in `src/capture/`
  - INPUT: `CaptureEngine` from T010
  - OUTPUT: Detect WGC frame size change, log event, route through GPU scaler to fixed 1920x1080 output. No encoder reset
  - VERIFY: Change display resolution during recording, verify video continues without corruption

- [ ] T035 [P] — Implement encoder software fallback chain in `src/encoder/`
  - INPUT: `VideoEncoder` from T013
  - OUTPUT: Deterministic fallback: HW MFT -> SW MFT (same res) -> 720p30 SW. Each attempt logged
  - VERIFY: Block HW encoder via mock, verify SW fallback activates and produces valid output

- [ ] T036 — Memory and queue stability review
  - INPUT: All modules
  - OUTPUT: Code review confirming: queues bounded at 5 frames, no unbounded allocations, COM pointers properly released
  - VERIFY: Run 10-minute recording, check memory graph for growth trends

---

## Phase 9: Production Grade Robustness & Telemetry

**Purpose**: Elevate the app from architecturally sound to operationally robust under sustained, heavy-usage conditions.

- [ ] T037 [P] — Implement runtime telemetry counters and debug overlay in `src/app/`
  - INPUT: All engine modules
  - OUTPUT: Counters: frames captured/encoded/dropped/backlogged. Optional debug overlay showing live stats
  - VERIFY: Record 30s, verify counters are non-zero and consistent

- [ ] T038 — Implement frame pacing normalization layer in `src/sync/`
  - INPUT: `SyncManager` from T014
  - OUTPUT: Absorb WGC timing jitter: smooth PTS intervals, insert duplicates for gaps > 1.5x target, drop newest on backpressure
  - VERIFY: Simulate jittery input (random ±10ms), verify output PTS is smooth

- [ ] T039 [P] — Implement D3D11 device-lost detection and recovery in `src/capture/`
  - INPUT: `CaptureEngine` from T010
  - OUTPUT: Handle `DXGI_ERROR_DEVICE_REMOVED`, recreate D3D11 device + frame pool, resume capture
  - VERIFY: Force device-lost via driver reset (or mock), verify capture resumes

- [ ] T040 — Implement periodic memory sampler and 60-min stress assertion in `tests/integration/`
  - INPUT: Built application
  - OUTPUT: Test harness: start recording, sample working set every 60s for 60 mins, assert < 5% growth
  - VERIFY: Test passes on target machine (i7-1255U, Iris Xe)

- [ ] T041 — Create 60-minute automated stability harness in `tests/integration/`
  - INPUT: Built application, video playback load generator
  - OUTPUT: Script: launch app, start recording, play 4K video in background, track CPU/RAM/drift on battery for 60 min
  - VERIFY: Frame drops < 5%, drift < 100ms, memory < 500 MB

- [ ] T042 [P] — Implement dynamic power-mode encoder adjustment in `src/encoder/`
  - INPUT: `VideoEncoder` from T013
  - OUTPUT: Detect AC/battery via `GetSystemPowerStatus`. On battery: default 1080p30/8Mbps. On AC: allow 1080p60/14Mbps
  - VERIFY: Unplug AC, verify encoder switches to 30fps preset

- [ ] T043 [P] — Implement WGC consent flow and denial handling in `src/app/`
  - INPUT: WGC API
  - OUTPUT: Proper GraphicsCapturePicker or programmatic consent. If denied, show clear error message and disable Start button
  - VERIFY: Deny permission, verify error message shown and app doesn't crash

---

## Dependencies & Execution Order

### Phase Dependencies

```text
Phase 1 (Setup)
  └── Phase 2 (Foundational) ── BLOCKS ALL user stories
        └── Phase 3 (US1 - MVP) ── BLOCKS US2, US3
              ├── Phase 4 (US2 - Pause) ── can parallel with US3
              ├── Phase 5 (US3 - Mute) ── can parallel with US2
              ├── Phase 6 (US4 - Settings) ── can start after US1
              └── Phase 7 (US5 - Fault Tolerance) ── can start after US1
        └── Phase 8 (Polish) ── can start once US1 core works
        └── Phase 9 (Robustness) ── can start once US1-US3 complete
```

### Task-Level Parallelism Within Phases

| Phase | Parallel Groups |
|-------|----------------|
| Phase 1 | T001 -> T002 -> {T003, T004} |
| Phase 2 | T005 -> T007; {T006, T008, T009} independently |
| Phase 3 | {T010, T011, T012} in parallel -> T013 -> T014 -> T015 -> T016 -> T017 |
| Phase 4 | T018 -> {T019, T020} -> T021 |
| Phase 5 | T022 -> T023 -> T024 |
| Phase 6 | {T025, T026} -> T027 |
| Phase 7 | {T028, T029} -> {T030, T031} |
| Phase 8 | All can run in parallel (independent modules) |
| Phase 9 | {T037, T038, T039, T042, T043} parallel -> {T040, T041} last |

### MVP Scope

**MVP = Phase 1 + Phase 2 + Phase 3 (Tasks T001-T017)**

At MVP completion, the user can:
- Launch the app
- Click Start to begin recording screen + mic
- Click Stop to finalize and save MP4
- Find the recording in `%USERPROFILE%\Videos\Recordings`
