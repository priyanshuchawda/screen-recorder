<!-- Sync Impact Report
Version change: 1.0.0 -> 1.1.0
Modified principles: 
- Combined Principle I and II into "Native Windows Performance & Direct GPU Pipeline"
- Added Principle II: "State Machine & Thread Ownership"
- Added Principle III: "Bounded Queues & Backpressure"
- Added Principle IV: "Encoder Fallback & Downgrade Policy"
- Renamed "Continuous Media Timeline" to Principle V, expanded pause/mute rules
- Renamed "Resilient Storage Handling" to Principle VI and added Crash Recovery behavior
Added sections: None
Removed sections: None
Templates requiring updates (✅ updated / ⚠ pending):
- .specify/templates/plan-template.md (✅ Checked, generic gates sufficient)
- .specify/templates/spec-template.md (✅ Checked, generic)
- .specify/templates/tasks-template.md (✅ Checked, generic)
Follow-up TODOs: None
-->

# Screen Recorder Constitution

## Core Principles

### I. Native Windows Performance & Direct GPU Pipeline
**MUST** use C++20, Win32, and C++/WinRT. Native wins on RAM/CPU usage and startup time compared to Electron or .NET. **MUST** use Windows Graphics Capture, D3D11, and Media Foundation H.264 HW encoding for a zero-copy GPU path, preventing CPU bottlenecks.

### II. State Machine & Thread Ownership
The `Session Controller` **MUST** act as the central state machine enforcing strict transitions: `Idle -> Recording -> Paused -> Recording -> Stopping -> Idle`. It owns the lifecycle of all engines.
Threading **MUST** be strictly separated into dedicated worker threads: UI thread, capture thread, audio capture thread, and encode/mux worker.

### III. Bounded Queues & Backpressure
**MUST** use bounded lock-free queues (small depth) to cap memory and latency.
**Frame Drop Policy**: System **MUST** drop the oldest pending video frame if the queue is full. It **MUST NEVER** block the capture callback.

### IV. Encoder Fallback & Downgrade Policy
The system **MUST** implement a strict encoder fallback chain if initialization or encoding fails:
- Attempt 1: H.264 hardware MFT.
- Attempt 2: H.264 software MFT at same resolution/FPS.
- Attempt 3: Auto-degrade to 720p30 software with a warning to the user.

### V. Continuous Media Timeline
**MUST** maintain a continuous MP4 timeline. Pauses require recording the `pause_start_qpc`, adding pause duration to `paused_accumulator`, rebasing outgoing timestamps (`effective_pts = raw_pts - paused_accumulator`), and forcing the next video frame as a keyframe. Mutes require injecting zeroed PCM silence.

### VI. Resilient Storage & Crash Recovery
**MUST** write to `.partial.mp4` during active recordings. On normal stop, rename to `.mp4`. On startup, the system **MUST** detect stale partial files and provide crash-recovery behavior by offering a "keep/delete/recover attempt" flow to the user.

## Performance & Resource Limits

Keep active buffering conservative; expected runtime RAM around 150–300 MB, with a strict maximum of 500 MB. Ensure battery power defaults to 1080p30, while AC allows 1080p60.

## Phased Development Workflow

Follow a phased approach:
1. Core capture + encode prototype
2. Audio + sync
3. Pause/Resume + mute/unmute
4. Robustness + storage
5. Performance tuning + QA

## Governance

All architectural changes **MUST** maintain the native zero-copy constraints. Adding heavy external dependencies (e.g., FFmpeg) **SHOULD NOT** be done unless native Media Foundation pathways absolutely fail.

**Version**: 1.1.0 | **Ratified**: 2026-02-24 | **Last Amended**: 2026-02-24
