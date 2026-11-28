# Event-Driven Media Foundation Encoder Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Execute each task with TDD and a focused commit.

**Goal:** Replace hardware MFT polling with event-driven asynchronous input/output scheduling while preserving accepted frames, bounding memory, and reporting terminal loss accurately.

**Architecture:** `IMFAsyncCallback` will complete each MFT event request and publish a bounded event signal. A single encoder worker owns all `IMFTransform` processing calls, consumes input credits and output events, and writes compressed samples to a bounded output queue consumed by the existing session/mux thread. The synchronous software fallback remains synchronous; M2 does not add an independent mux writer.

**Tech Stack:** C++20, Media Foundation asynchronous MFT interfaces, WRL COM ownership, D3D11 surface-backed input samples, GoogleTest.

**Spec:** User's M2 requirements in the 2026-11-27 session request; M1 baseline PR #163 and recording diagnostics `ScreenRec_2026-11-27_15-50-58.diagnostics.txt`.

## Global Constraints

- Keep native C++20, D3D11, Media Foundation, Intel Quick Sync, and WASAPI.
- Do not increase the raw capture queue capacity to hide encoder stalls.
- Keep the input, event, pending-frame, and compressed-output paths explicitly bounded.
- Preserve tracked GPU surface lifetime through asynchronous input acceptance.
- Never silently discard accepted inputs or compressed outputs; classify and surface terminal loss.
- Keep the existing software fallback and synchronous MFT behavior available.
- Do not implement M3's independent mux writer, adaptive frame cadence, new codecs, or performance claims here.

## Review Focus

1. MFT emits `METransformNeedInput` before a frame is queued — the later submission must use the saved credit without polling.
2. MFT emits multiple NeedInput/HaveOutput events — each credit is consumed once and outputs are drained in order.
3. `ProcessInput` returns `MF_E_NOTACCEPTING` — the exact sample and metadata remain pending for a later credit.
4. Output queue reaches its packet/byte cap — the worker must retain the output event and apply bounded backpressure without dropping the sample.
5. Drain, device loss, and shutdown race with a pending event callback — all accepted inputs resolve as output, recovered retry, or explicit abandonment, and callbacks/workers terminate safely.

## Tasks

### Task 1: Testable bounded asynchronous scheduler

**Files:** Create `src/encoder/async_mft_scheduler.h`; test `tests/unit/test_async_mft_scheduler.cpp`; register through existing `CONFIGURE_DEPENDS` discovery.

- [x] Add scheduler tests for queued inputs, single-use credits, retries, queue/byte limits, drain completion, failure, and shutdown.
- [x] Reserve one queue slot while an input submission is outstanding so MF_E_NOTACCEPTING can always return the same input.
- [x] Separate worker and output-consumer condition variables; test that output publication does not wake the worker waiter.
- [x] Test that an idle worker wakes once for an enqueued input without a NeedInput credit and then waits for the credit.
- [x] Test watchdog deadline start after idle, timeout, reset after idle, and progress renewal.
- [x] Implement bounded state transitions and condition-variable notification with no normal-operation timer polling.
- [x] Run focused scheduler tests. The scheduler is included in the current M2 candidate commit, not split into earlier commits.

### Task 2: Media Foundation event callback

**Files:** Modify `src/encoder/video_encoder.h` and `.cpp`; add a focused callback helper under `src/encoder/` only if it materially isolates COM lifetime management; extend unit tests for mailbox ordering and callback termination state.

- [x] Verify the async MFT attribute, unlock it, query `IMFMediaEventGenerator`/`IMFShutdown`, and check initialization HRESULTs.
- [x] Subscribe with BeginGetEvent; call EndGetEvent exactly once per request; capture stream IDs/status; rearm after each nonterminal event.
- [x] Make drain-complete disarm event re-registration and wait for callback quiescence during shutdown.
- [x] Test callback rearming and drain-complete behavior using a real Media Foundation event queue.
- [x] Give each callback shared ownership of its scheduler generation and test that a late event cannot reach the next candidate's scheduler.

### Task 3: Single-owner hardware encoder worker

**Files:** Modify `src/encoder/video_encoder.h` and `.cpp`, using the scheduler from Task 1.

- [x] Add scheduler-level fake async-MFT protocol tests for delayed/multiple credits, rejection retry, output backpressure, failure, and shutdown. These test the scheduler protocol, not the full VideoEncoder worker against a fake IMFTransform.
- [x] Start one worker that alone calls transform ProcessInput, ProcessOutput, stream messages, and drain; callback threads only EndGetEvent and signal.
- [x] Submit tracked-surface samples against NeedInput credits, with a four-input in-flight limit and bounded four-sample application queue.
- [x] Store frame ID, PTS, and synthetic-frame status with each accepted input; match output metadata by returned PTS where available.
- [x] Publish compressed output in a bounded queue (maximum 16 samples and 8 MiB of allocated sample-buffer capacity); hold HaveOutput credit until capacity exists.
- [x] Measure event wait, queue residence, ProcessInput/ProcessOutput duration, accepted-to-output latency, worker wakeups, and high-water marks using bounded histograms/counters.
- [x] Run focused scheduler and callback tests. Hardware MFT behavior remains unverified on this host.

### Task 4: Session output consumption and deterministic drain

**Files:** Modify `src/controller/session_controller.cpp` and encoder diagnostics structures; test the output-consumption and drain behavior.

- [x] Consume async outputs from the existing session/mux loop even when no new capture frame arrives.
- [x] Drain queued outputs while waiting for MFT drain completion so output backpressure does not deadlock stop.
- [x] Track frame IDs and synthetic status through normal and final-drain mux writes; reconcile pending/abandoned counts.
- [x] Explicitly release queued compressed outputs after shutdown and account them as losses.
- [x] Allocate caller-provided output buffers using the MFT's `cbAlignment` requirement.
- [x] Treat drain timeout, event failure, output overflow, device removal, and shutdown failure as incomplete and release callback/worker/surface ownership.
- [ ] Add full VideoEncoder/session integration tests using a fake IMFTransform. Current protocol tests cover scheduler and the real MF event queue separately.

### Task 5: Full verification and hardware candidate

**Files:** Update diagnostics/README only where behavior changed; no unrelated documentation edits.

- [x] Fresh Debug and Release builds; full CTest suites (246/246 each); cppcheck and `git diff --check` after review corrections.
- [x] Review queue/surface budgets, COM ownership, stop/fallback behavior, and counter transitions at source level.
- [x] Build a separate Release candidate; document that hardware behavior requires manual 720p30 and 1080p60 validation.
- [x] Initial M2 candidate commit `3144e66` and PR #164 were pushed before review; follow-up correction commit is ready to push after final verification.

## Execution Ledger

- M1 baseline review: PR #163 merged as `25f8396`; Debug and Release CTest were each 220/220 before merge.
- Hardware baseline: 9m32 at 720p30/5 Mbps on AC; 68 capture queue drops; encoder drain complete; recording status incomplete. Average `encode_frame` wall time includes current polling/waits and does not isolate hardware execution.
- Review follow-up: watchdog now starts on the pending-work transition and clears after idle; callback state is generation-owned; rejected submissions retain reserved queue capacity; worker and consumer waiters have separate condition variables; drain metadata and aligned MFT output buffers are preserved. In-process driver calls can still make a join unbounded; README documents the process-isolation limitation.
