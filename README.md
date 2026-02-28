# ScreenRecorder

A lightweight, high-performance native Windows screen recorder built with C++20 and Windows-native APIs.

![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![License](https://img.shields.io/badge/License-Private-red)
![Status](https://img.shields.io/badge/Status-Active%20Development-green)
![Tests](https://img.shields.io/badge/Tests-72%20passing-brightgreen)

## Overview

A minimal, fast, and efficient screen recording application for Windows that captures the full screen with microphone audio, supports pause/resume and mute/unmute, and automatically saves recordings to a local directory. Built entirely with native Windows APIs for maximum performance and minimal resource usage.

## Features

- **Full-Screen Capture** — Windows Graphics Capture API with zero-copy GPU pipeline
- **Hardware-Accelerated Encoding** — H.264 via Intel Quick Sync (Media Foundation)
- **Microphone Audio** — WASAPI event-driven capture with AAC-LC encoding
- **Pause & Resume** — Seamless timestamp rebasing, no file corruption
- **Mute & Unmute** — Silence injection maintains continuous timeline
- **Settings Panel** — FPS preset (30/60), output directory, persisted to INI
- **Auto-Save** — Recordings saved to `%USERPROFILE%\Videos\Recordings`
- **Crash Recovery** — `.partial.mp4` temp files with startup recovery dialog (Recover / Delete / Ignore)
- **Low-Disk Auto-Stop** — Background polling stops recording gracefully when disk < 500 MB free
- **Exclusive File Lock** — `.partial.mp4` allows readers but blocks external writers
- **Low Resource Usage** — Target < 500 MB RAM, < 5% frame drops at 1080p60

## Tech Stack

| Component | Technology |
|-----------|-----------|
| **Language** | C++20 |
| **UI** | Win32 + C++/WinRT |
| **Screen Capture** | Windows Graphics Capture API (WGC) |
| **Video Encode** | Media Foundation H.264 HW MFT (Intel Quick Sync) |
| **Audio Capture** | WASAPI (IAudioClient3, event-driven shared mode) |
| **Audio Encode** | AAC-LC via Media Foundation |
| **Container** | MP4 via IMFSinkWriter |
| **Color Convert** | D3D11 Video Processor (BGRA → NV12, GPU-only) |
| **Build System** | CMake + Visual Studio 2022 (MSBuild) |

## Architecture

```
[UI Layer (Win32/C++/WinRT)]
        │
        ▼
[Session Controller + State Machine]
        │
        ├─────────────────────────┐
        ▼                         ▼
[Video Capture Engine]       [Audio Engine (WASAPI)]
(WGC → D3D11 Texture)       (PCM packets, event-driven)
        │                         │
        ▼                         │
[GPU Preprocess]                  │
(BGRA → NV12, GPU scaler)        │
        │                         │
        └───────────┬─────────────┘
                    ▼
        [Timestamp/Sync Manager]
                    │
                    ▼
      [Encoder + Mux Pipeline]
 (H.264 HW MFT + AAC → MP4 SinkWriter)
                    │
                    ▼
            [Storage Manager]
      (async IO, .partial.mp4 → .mp4)
```

## Project Structure

```
src/
├── app/           # UI Layer, main entry point, WGC consent
├── controller/    # Session Controller (State Machine)
├── capture/       # Capture Engine (WGC) + GPU Preprocess
├── audio/         # Audio Engine (WASAPI, silence injection)
├── encoder/       # Encoder/Mux (Media Foundation H.264/AAC)
├── sync/          # Sync Manager (QPC clock, PTS alignment)
├── storage/       # Storage Manager (async IO, disk monitoring)
└── utils/         # Bounded queues, threading, QPC wrappers

tests/
├── unit/          # State machine, queue, sync logic tests
└── integration/   # Fallback, file lock, 60-min stress tests

specs/
└── 001-screen-recorder/   # Feature specification & planning docs
    ├── spec.md             # Requirements & acceptance criteria
    ├── plan.md             # Implementation plan
    ├── tasks.md            # Task breakdown (43 tasks, 9 phases)
    ├── research.md         # Technology decisions
    ├── data-model.md       # State machines & data types
    └── quickstart.md       # Build instructions
```

## Prerequisites

- Windows 10/11 (64-bit, Build 17134+)
- Visual Studio 2022+ with "Desktop development with C++" workload
- Windows SDK 10.0.26100+
- CMake 4.0+ and Ninja
- Hardware with D3D11 support (Intel Quick Sync recommended)

## Building

```powershell
# Configure (first time)
cmake -B build -G "Visual Studio 18 2026" -A x64

# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release

# Run tests
.\build\tests\Debug\unit_tests.exe

# Run the app
.\build\Debug\ScreenRecorder.exe
```

> ⚠️ **Note**: Use the Visual Studio generator — Ninja fails on this project due to `rc.exe` path issues. See `BUILD_NOTES.md` for details.

## Development Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Project setup & CMake | ✅ Complete |
| Phase 2 | Core infrastructure (queues, state machine) | ✅ Complete |
| Phase 3 | **MVP** — Screen + mic capture, encode, save | ✅ Complete |
| Phase 4 | Pause & Resume | ✅ Complete |
| Phase 5 | Mute & Unmute | ✅ Complete |
| Phase 6 | Settings (FPS preset, output dir, INI persistence) | ✅ Complete |
| Phase 7 | Fault tolerance & crash recovery | ✅ Complete |
| Phase 8 | Polish & edge cases | 🔄 Up next |
| Phase 9 | Robustness & telemetry | ⬜ Not started |

## Target System

Developed and tested on:
- **Dell Inspiron 15 3520**
- Intel i7-1255U (10C/12T, Alder Lake-U)
- Intel Iris Xe Graphics (Quick Sync)
- 16 GB RAM
- Windows 11 Home (Build 26200)

## License

Private — All rights reserved.
