# M3 Independent Media Writer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Media Foundation sink writes to a dedicated worker and absorb short storage stalls with bounded buffering of encoded video and audio samples.

**Architecture:** The controller submits immutable `IMFSample` references to one bounded FIFO owned by `MuxWriter`. The worker initializes COM and creates, configures, writes, finalizes, and releases the sink writer in its own MTA; the interface is never marshaled across apartments. Finalize closes submissions, drains accepted packets, joins the worker after its sink finalization, and renames only if every accepted packet was written. Limits cover packet count, total queued bytes and per-stream media-time span. Capacity overflow or asynchronous write failure is surfaced as recording loss; accepted packets are never silently evicted.

**Tech Stack:** Native C++20, Win32 threads/condition variables, Media Foundation, existing GoogleTest/CMake setup.

**Spec:** M3 independent writer and bounded compressed-media buffering requested in conversation; M2 candidate at `18fee49`.

## Global Constraints

- Preserve native C++20, D3D11, Media Foundation, WASAPI and Intel Quick Sync.
- Do not increase raw capture or encoder queue capacities to mask stalls.
- Keep compressed media buffering bounded; do not silently discard accepted media.
- Preserve current stop ordering: stop producers and encoder, then drain writer, then finalize MP4.
- Keep M2 as the parent; do not merge PR #164 as part of this implementation.
- Keep the desktop shortcut on M2 until M3 builds and automated tests pass; then install M3 candidate for user hardware validation.

## Review Focus

- Writer stalled beyond limits: enqueue must fail visibly, remain bounded and classify recording incomplete.
- Finalize with queued packets: all accepted packets must drain before `IMFSinkWriter::Finalize`.
- WriteSample failure: wake waiters, reject new samples and retain partial output without renaming.
- Concurrent audio/video submissions: preserve FIFO submission order and per-stream timestamp order.
- Startup rollback and repeated sessions: stop/join writer and release samples on every path.

## Task 1: Bounded media packet queue

**Files:**
- Create: `src/storage/bounded_media_queue.h`
- Test: `tests/unit/test_bounded_media_queue.cpp`
- Modify: `tests/CMakeLists.txt` only if source discovery does not include the new test automatically.

- [ ] Write tests for byte limit, per-stream duration span, mixed-stream accounting, close-and-drain, rejection after close, high-water marks and concurrent producers/consumer.
- [ ] Run the focused test and confirm the new tests fail before implementation.
- [ ] Implement a bounded FIFO that accepts `IMFSample` references plus stream, byte length, PTS and duration; reject newest packet on capacity overflow without evicting existing media.
- [ ] Run focused tests and complete unit suite.
- [ ] Commit the queue and tests.

## Task 2: Dedicated Sink Writer worker

**Files:**
- Modify: `src/storage/mux_writer.h`
- Modify: `src/storage/mux_writer.cpp`
- Modify: `tests/unit/test_mux_writer.cpp`

- [ ] Add explicit queue limits (initially 16 MiB total queued samples and 5 seconds of queued PTS span per stream), queued/peak bytes and duration, queue high-water sample count, write failures and writer state.
- [ ] Start a single writer worker only after `BeginWriting` succeeds. It exclusively calls `WriteSample` and serializes audio/video FIFO packets.
- [ ] Make `write_video`/`write_audio` enqueue nonblocking and fail on closed, full or invalid samples; count accepted samples separately from completed writes.
- [ ] On worker write failure, set terminal error, stop accepting, release queued samples, wake all waiters and keep the partial file.
- [ ] Make `finalize` close submissions, drain accepted packets, join worker, then finalize/rename only on a successful drain and MF finalization. `abort` closes, clears, joins and resets idempotently.
- [ ] Keep all failure paths safe when initialization only partially completes.
- [ ] Add deterministic tests around queue behavior and worker lifecycle; add an injectable writer-call seam if required to test simulated delay/failure without production fault hooks.
- [ ] Build Debug and Release and run all CTest tests.
- [ ] Commit the dedicated writer.

## Task 3: Controller integration and diagnostics

**Files:**
- Modify: `src/controller/session_controller.cpp`
- Modify: `src/controller/session_controller.h` only for new timing/state snapshots.
- Modify: `src/utils/session_diagnostics.*`
- Modify: README recording diagnostics section.

- [ ] Update video/audio mux timing to distinguish enqueue latency from actual writer service latency; include final drain and finalize operations.
- [ ] Poll writer terminal state from the controller and request safe stop on permanent writer failure; queue capacity pressure marks incomplete and stops recording without losing the existing partial file.
- [ ] Ensure final status is complete only when every accepted media packet has been written and sink finalization succeeds.
- [ ] Log queue bytes, per-stream queued duration, packet count/high-water marks, enqueue rejects, writer stalls, write failures and worker join/drain duration.
- [ ] Add tests for controller-visible writer failure and final status if test seams permit.
- [ ] Run fresh Debug/Release configure, build, full CTest, static analysis and `git diff --check`.
- [ ] Commit integration and diagnostics.

## Task 4: Candidate and hardware validation handoff

- [ ] Build a separate Release candidate and record its commit SHA and SHA256.
- [ ] Replace desktop shortcut target with the candidate after automated validation passes.
- [ ] User validates 720p30 for 3–5 minutes, including ordinary screen movement and audio, then provides MP4 and diagnostics.
- [ ] Do not claim storage-stall recovery or performance improvements until measured with a controlled injected stall and real hardware recording.
- [ ] Open an M3 pull request targeting `feat/m2-async-mft` while PR #164 remains open; retarget after M2 is merged.

## Approaches

- **Queue `IMFSample` references (selected):** Avoid extra media copies and keep compressed output and PCM audio sample buffers intact. The writer worker alone calls the sink; samples are immutable after enqueue and held by COM references until completion. This matches existing sample ownership and minimizes added CPU work.
- Copy sample payloads into byte packets and rebuild samples on the writer thread: improves COM object thread isolation but adds per-packet allocation/copy and sample reconstruction. Not selected unless runtime testing shows cross-thread sample ownership issues.
- Disable Sink Writer throttling or buffer unboundedly: rejected because it can move backpressure into unbounded memory and conceal disk stalls.

## Exit Criteria

- All automated tests pass in Debug and Release.
- Every accepted packet is either confirmed written or explicitly reported lost; no silent queue eviction.
- Queue remains within byte and duration limits under sustained writer delay.
- Finalized MP4 decodes and is only renamed after successful writer drain and `Finalize`.
- M3 hardware candidate test results are reported separately from M2 baseline; no battery or throughput claims without matched measurements.
