# Feature Specification: Full Screen Recorder

**Feature Branch**: `001-screen-recorder`
**Created**: 2026-02-24
**Updated**: 2026-02-24
**Status**: Approved
**Input**: User description + system capability analysis

---

## Target System Profile

| Property | Value |
|----------|-------|
| **OS** | Windows 11 Home (Build 26200, 64-bit) |
| **CPU** | 12th Gen Intel Core i7-1255U (10C/12T, 1.7 GHz base, Alder Lake-U, L2: 6.5 MB, L3: 12 MB) |
| **RAM** | 16 GB |
| **GPU** | Intel Iris Xe Graphics (2 GB shared VRAM, Quick Sync confirmed via `libmfxhw64.dll`) |
| **Display** | 1920x1080 native panel, 1536x864 logical resolution, DPI: 96 (100%) |
| **Monitors** | Single built-in display |
| **Disk** | ~930 GB NTFS, ~182 GB free (C: drive, 19.6% free) |
| **Audio** | Intel SST (Bluetooth + USB), Cirrus Logic HD Audio (all status: OK) |
| **Power Plan** | Balanced (laptop, battery-powered) |
| **Machine** | Dell Inspiron 15 3520 (laptop) |
| **MSVC** | VS Community 2026 (v18.0), MSVC Toolset 14.50.35717, Windows SDK 10.0.26100 |
| **Build Tools** | CMake 4.1.2, Ninja 1.13.2, Git 2.51.2 |
| **Media Foundation** | mfplat.dll v10.0.26100.4768, mfreadwrite.dll v10.0.26100.7705, Intel QSV present |

---

## Clarifications

### Session 2026-02-24

- Q: How does the system handle resolution changes (e.g., connecting an external monitor) while a recording is in progress?
  A: Scale to a fixed target on the GPU (e.g., 1920x1080) to preserve the continuous MP4 timeline without resetting the encoder.

- Q: What happens when the user's microphone is physically unplugged during an active recording?
  A: Inject silence (zeroed PCM), show a non-blocking UI warning, and allow the user to seamlessly rebind without interrupting the video timeline or encoder.

- Q: What happens when a user attempts to play a partially written temporary file in an external media player while recording is active?
  A: Set exclusive write locks (Allow Read) to prevent interference from external processes while still granting read access without throwing harsh OS-level errors.

- Q: The display reports 1536x864 logical resolution vs 1920x1080 native. How should the capture handle this?
  A: System DPI is 96 (100%). The 1536x864 is a Windows display scaling setting. WGC captures at the actual render resolution. The GPU preprocess step normalizes to 1920x1080 output if needed. No special action required for single-monitor full-screen capture.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Basic Screen and Microphone Recording (Priority: P1)

Users need to quickly start and stop a screen recording with their microphone audio included to create tutorials or presentations.

**Why this priority**: Core functionality; without the ability to start and stop a recording, the application has no value.

**Independent Test**: Launch the app, start a recording, speak into the microphone, stop the recording, and verify the resulting video file plays correctly with synchronized audio.

**Acceptance Scenarios**:

1. **Given** the application is idle, **When** the user clicks "Start", **Then** the UI updates to show a recording state, a timer begins, and a temporary recording file is created.
2. **Given** an active recording, **When** the user clicks "Stop", **Then** the timer halts, the UI returns to idle, and the final video file is saved and ready for playback.

---

### User Story 2 — Pause and Resume Recording (Priority: P2)

Users need to pause their recording to gather thoughts or transition between topics without resulting in multiple separate video files.

**Why this priority**: Highly requested quality-of-life feature that significantly reduces the need for post-processing and editing.

**Independent Test**: Start a recording, pause it, wait a few seconds, resume, stop the recording, and verify the final video playback is continuous without out-of-sync audio or visual corruption.

**Acceptance Scenarios**:

1. **Given** an active recording, **When** the user clicks "Pause", **Then** the recording duration timer pauses, and new visual/audio input stops being added to the file.
2. **Given** a paused recording, **When** the user clicks "Resume", **Then** the timer continues, and the new visual/audio input is appended seamlessly to the existing timeline.

---

### User Story 3 — Mute and Unmute Microphone (Priority: P2)

Users may need to temporarily silence their microphone during a recording (e.g., to cough or block background noise) without stopping the video capture.

**Why this priority**: Crucial for professional recordings where unexpected interruptions occur.

**Independent Test**: Start a recording, mute the microphone, speak, unmute, speak again, review the final video, and confirm silence during the muted portion while video remains continuous.

**Acceptance Scenarios**:

1. **Given** an active recording with audio, **When** the user clicks "Mute", **Then** the microphone audio is replaced by silence in the recording, but the visual capture continues uninterrupted.
2. **Given** a muted recording, **When** the user clicks "Unmute", **Then** the microphone audio is cleanly reintroduced into the recording timeline.

---

### User Story 4 — Configuration Settings (Priority: P3)

Users with different hardware capabilities or storage constraints need to adjust the recording frame rate and output location.

**Why this priority**: Necessary for accommodating varying user systems, but secondary to the core recording flow.

**Independent Test**: Change the FPS setting and output folder, perform a recording, and verify the resulting file's properties and location match the new settings.

**Acceptance Scenarios**:

1. **Given** the settings menu, **When** the user changes the FPS preset from 30 to 60, **Then** subsequent recordings are captured at the higher frame rate.
2. **Given** the settings menu, **When** the user changes the output directory, **Then** all future finalized recordings are saved in the newly specified folder.

---

### User Story 5 — Fault Tolerance and Crash Recovery (Priority: P3)

Users need assurance that long recordings will not be lost entirely due to sudden app crashes or running out of disk space.

**Why this priority**: Prevents severe data loss and negative user experiences, building trust in the tool.

**Independent Test**: Simulate low disk space during a recording to verify graceful auto-stop, and force the process to crash to verify the recovery prompt on next startup.

**Acceptance Scenarios**:

1. **Given** an active recording, **When** the system detects critically low available disk space, **Then** the application notifies the user and automatically finalizes the recording gracefully.
2. **Given** the application is launched after an abnormal exit, **When** a stale temporary recording file is found, **Then** the application prompts the user with options to attempt recovery of the file or delete it.

---

### Edge Cases

- When a user's microphone is physically unplugged during an active recording, the system injects silence to maintain timeline continuity and allows for a seamless rebind once reconnected.
- When a user attempts to start a recording but the output directory is read-only or deleted, the system shows a clear error and falls back to the default directory.
- When a user attempts to open a partially written temporary file in an external program, the system permits read access to avoid OS-level errors but maintains an exclusive write lock to prevent interference with encoder stability.
- When a user changes display resolution or connects an external monitor mid-recording, the system continues feeding frames through a GPU scaler to a fixed output (e.g., 1920x1080) preventing encoder reset or timeline discontinuity.
- When the system is on battery power (Balanced plan), the default recording preset should be conservative (1080p30) to avoid thermal throttling on the i7-1255U.
- When DPI scaling is active (125% on this machine), WGC should capture at native pixel resolution (1920x1080), not the logical resolution (1536x864).

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST capture the full screen and microphone audio continuously.
- **FR-002**: System MUST provide an interface with controls to Start, Pause, Resume, and Stop recording.
- **FR-003**: System MUST provide an interface to toggle the microphone mute state during an active recording.
- **FR-004**: System MUST display the elapsed recording duration, current FPS, dropped frames, and the save destination path.
- **FR-005**: System MUST write active recording data to a temporary file format, only renaming it to the final format upon a successful "Stop" action.
- **FR-006**: System MUST automatically halt the recording and save the file if available disk space falls below 500 MB.
- **FR-007**: System MUST detect orphaned temporary files on application startup and prompt the user with a recovery or deletion flow.
- **FR-008**: System MUST allow users to select between distinct FPS presets (e.g., 30 FPS and 60 FPS).
- **FR-009**: System MUST allow users to specify a custom output directory for finalized recordings.
- **FR-010**: System MUST maintain audio-video synchronization without drift, even after pause/resume or mute/unmute events.

---

### Technical Constraints (Implementation-Specific Rules)

> **Note**: These constraints are intentionally implementation-specific. They prescribe the exact Windows-native APIs and configurations chosen for this project based on the target system profile above. This is by design — the project is self-driven and the tech stack has been pre-decided.

- **TC-001**: Screen capture MUST use Windows Graphics Capture API.
- **TC-002**: Video encoding MUST use Media Foundation H.264 with hardware acceleration when available (Intel Quick Sync on Iris Xe).
- **TC-003**: Video frames MUST remain GPU-backed (ID3D11Texture2D) until submission to encoder.
- **TC-004**: Audio capture MUST use WASAPI in shared event-driven mode.
- **TC-005**: Pause MUST NOT tear down encoder or sink writer.
- **TC-006**: Mute MUST inject silence samples instead of dropping audio frames.
- **TC-007**: Video capture callbacks MUST NEVER block on encoder queue.
- **TC-008**: Video frame queue MUST be bounded (max 5 frames). Audio queue MUST never block capture thread.
- **TC-009**: If hardware encoder initialization fails, system MUST attempt software fallback with the following deterministic order: Attempt 1: H.264 Hardware MFT -> Attempt 2: H.264 Software MFT -> Attempt 3: Auto-degrade to 720p30 Software.
- **TC-010**: The application MUST remain single-process.
- **TC-011**: The system MUST detect frame size changes from WGC, log the event, and scale frames to a fixed output on the GPU without resetting the encoder or session state.
- **TC-012**: On audio device disconnection, system MUST immediately switch to synthetic silence mode (pushing zeroed PCM buffers with correct timestamps) without resetting the session clock, flushing the encoder, or altering PTS continuity.
- **TC-013**: The system MUST open the temporary `.partial.mp4` file with an exclusive write lock (GENERIC_WRITE, FILE_SHARE_READ, deny FILE_SHARE_WRITE) to prevent corruption from external processes.
- **TC-014**: System MUST implement explicit frame pacing: if capture delta > 1.5x frame interval, insert a duplicate frame. If queue is full, drop the NEWEST frame (not oldest).
- **TC-015**: GPU shader mapping OR D3D11 Video Processor MUST be used to convert BGRA (from WGC) to NV12 without CPU roundtripping.
- **TC-016**: H.264 Encoder MUST be configured explicitly for: CBR, No B-frames, GOP = 2 seconds, Low-latency mode enabled, Baseline/Main profile.
- **TC-017**: Hardware encoder detection MUST enumerate MFTs natively (`MFT_ENUM_FLAG_HARDWARE`), validate NV12 support, and validate D3D11 device manager support before instantiation.
- **TC-018**: WGC item selection MUST lock to a specific monitor, cache DPI transform, and safely handle mid-session multi-monitor hotplugs. To prevent tearing from mid-frame captures, it MUST set `IsCursorCaptureEnabled`, `SetBorderRequired(false)`, and recreate the frame pool on resolution changes.
- **TC-019**: Audio processing MUST implement an `MFResampler` to accommodate sample rate mismatches to prevent drift, and MUST handle device invalidation callbacks gracefully.
- **TC-020**: The storage manager MUST employ an async disk writing thread using IO completion ports or overlapped writes to decouple muxing latency from slow disk speeds, and strictly monitor disk free space via `GetDiskFreeSpaceEx`.
- **TC-021**: The system MUST implement explicit power/system optimizations: `SetThreadPriority`, `SetProcessPriorityClass(ABOVE_NORMAL)`, and `MMCSS` registration for the audio thread.
- **TC-022**: WGC MUST capture at native pixel resolution (1920x1080) regardless of system DPI scaling factor (125% on this machine). The capture must be DPI-aware.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The application launches and is ready to record in under 2 seconds.
- **SC-002**: Users can seamlessly pause and resume a recording without introducing audio desync or timeline corruption in the final video.
- **SC-003**: The resulting video file maintains synchronized audio and video (drift < 100ms) over a continuous 60-minute recording session.
- **SC-004**: The system successfully recovers at least 95% of the recorded footage from a temporary file following a simulated application crash.
- **SC-005**: Memory consumption remains bounded and does not grow indefinitely during extended recording sessions (no memory leaks).
- **SC-006**: The application detects a low-disk space condition and safely finalizes the video file 100% of the time before encountering write errors.
- **SC-007**: Video frame drops must remain below 5% during 1080p60 recording on supported hardware under normal thermal conditions.
- **SC-008**: Memory usage after 60-minute recording must not exceed initial steady-state memory by more than 5% (target: < 500 MB total).
