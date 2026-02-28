# Quickstart: Screen Recorder App

## Target System (Verified 2026-02-24)

| Property | Value |
|----------|-------|
| Machine | Dell Inspiron 15 3520 |
| OS | Windows 11 Home (Build 26200) |
| CPU | Intel i7-1255U (Alder Lake-U, 10C/12T) |
| GPU | Intel Iris Xe Graphics (Quick Sync confirmed) |
| Display | 1920x1080 native, 1536x864 logical |
| MSVC | VS Community 2026 (v18.0), Toolset 14.50.35717 |
| SDK | Windows SDK 10.0.26100 |

## Prerequisites

All verified as installed on this machine:

- [x] **Visual Studio 2026 Community** with "Desktop development with C++" workload
  - Install path: `C:\Program Files\Microsoft Visual Studio\18\Community`
  - MSVC Toolset: `14.50.35717`
- [x] **Windows SDK 10.0.26100** — provides C++/WinRT, WGC, Media Foundation, D3D11 headers
  - All critical headers verified: `d3d11.h`, `mfapi.h`, `audioclient.h`, `windows.graphics.capture.h`
  - All critical libs verified: `d3d11.lib`, `dxgi.lib`, `mfplat.lib`, `mfuuid.lib`, `mfreadwrite.lib`, `windowsapp.lib`
  - C++/WinRT projection headers present
  - Build tools: `rc.exe`, `mt.exe`, `cppwinrt.exe`
- [x] **CMake 4.1.2** — `C:\Program Files\CMake\bin\cmake.exe`
- [x] **Ninja 1.13.2** — `C:\Users\Admin\AppData\Local\Microsoft\WinGet\Links\ninja.exe`
- [x] **Git 2.51.2** — `C:\Program Files\Git\cmd\git.exe`
- [x] **Python 3.14.3** — `C:\Python314\python.exe` (for test scripts)
- [x] **Intel Quick Sync** — `libmfxhw64.dll` present in System32
- [x] **Media Foundation** — `mfplat.dll` v10.0.26100.4768

## Building

### Option A: Ninja (Recommended — fastest)

```powershell
# Navigate to project root
cd C:\Users\Admin\Desktop\screen-recorder

# Configure (first time only)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Release build
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
```

### Option B: Visual Studio Generator

```powershell
# Configure
cmake -B build -G "Visual Studio 18 2026" -A x64

# Build Debug
cmake --build build --config Debug

# Build Release
cmake --build build --config Release
```

### Option C: Manual build.bat (Fallback)

See `path.md` for manual PATH/INCLUDE/LIB environment setup if CMake fails.

## Running the App

```powershell
# Debug
.\build\ScreenRecorder.exe

# Release
.\build-rel\ScreenRecorder.exe
```

A minimal UI window will open. Click **Start** to begin recording. The output will automatically go to `%USERPROFILE%\Videos\Recordings`.

## Running Tests

```powershell
# Run all tests
ctest --test-dir build --output-on-failure

# Run specific test suite
.\build\tests\unit_tests.exe --gtest_filter="SessionMachine*"
```

## Environment Expectations

- The application requires D3D11 and hardware video encoding for optimal 1080p60 performance
- Intel Quick Sync (`libmfxhw64.dll`) is confirmed on this machine
- On systems without hardware encoder, the app falls back to software encoding (may reduce to 720p30)
- Balanced power plan is active; on battery, the app defaults to 1080p30 to manage thermals
- At least 500 MB free disk space required to start a recording
- Output directory: `%USERPROFILE%\Videos\Recordings` (created automatically if missing)

## Project Structure

```text
src/
├── app/           # UI Layer, main entry point, WGC consent
├── controller/    # Session Controller (State Machine)
├── capture/       # Capture Engine (WGC) + GPU Preprocess
├── audio/         # Audio Engine (WASAPI)
├── encoder/       # Encoder/Mux (Media Foundation)
├── sync/          # Sync Manager
├── storage/       # Storage Manager (Async IO)
└── utils/         # Queues, threading, QPC wrappers

tests/
├── unit/          # State machine, queue, sync tests
└── integration/   # Fallback, file lock, stress tests
```

## Verification

Run the deep system scan to verify your environment:

```powershell
powershell -ExecutionPolicy Bypass -File deep-sysinfo.ps1
```

All items should show `[OK]`. Any `[MISSING]` items need to be resolved before building.
