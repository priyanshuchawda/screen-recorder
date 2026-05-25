# ScreenRecorder

Lightweight native Windows screen recorder built with C++20, Win32, Windows Graphics Capture, Media Foundation, D3D11, Intel Quick Sync capable hardware encoding, and WASAPI.

![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green)
![Status](https://img.shields.io/badge/Status-Stable-green)
![Tests](https://img.shields.io/badge/Tests-138%20passing-brightgreen)

Website: [screen-recorder-windows.netlify.app](https://screen-recorder-windows.netlify.app)

## Features

- Full-screen capture via Windows Graphics Capture (WGC)
- Hardware-first H.264 video + AAC audio muxed to MP4 using Media Foundation
- Intel GPU / Quick Sync preference when available, with graceful software fallback
- **System Audio Capture**: Record desktop/YouTube audio via WASAPI Loopback
- **Microphone Noise Gate**: RMS-based gating to eliminate background hiss
- **Camera Overlay**: Throttled efficient default preview with an HQ 720p-capable profile
- **Anti-Ducking**: Opt-out from Windows auto-lowering volume during capture
- **High Quality Mode**: Optional 1080p-capable hardware profile with higher bitrate recording (8/10 Mbps) on AC or battery
- **Recording Diagnostics**: Per-session local `.diagnostics.txt` files show adapter, encoder mode, power state, profile, and completion counters
- Embedded app icon for the main and settings windows
- Pause/resume with monotonic timestamp rebasing
- Mute/unmute with silence injection
- Recovery flow for orphaned `.partial.mp4` files
- Low-disk auto-stop and output directory management

## Performance Profiles

- **Default mode** targets low RAM and battery use: fixed 848x480 recording target, bounded frame queues, hardware-first encoding, and battery-aware throttling.
- **High Quality mode** is opt-in: 1080p-capable recording with higher bitrate and HQ camera preview on AC or battery.
- **Camera preview** intentionally uses a throttled RGB32/GDI overlay path today. It avoids adding a second GPU composition pipeline, keeps fallback simple across webcams, and is rate-limited to reduce CPU/battery cost. Screen capture and video encoding still use the D3D11/Media Foundation hardware path when available.
- Every recording writes a small diagnostics file beside the MP4 so the selected adapter, encoder mode (`HW`, `SW`, or fallback), power state, profile, and completion status can be verified after the run.

## Build Requirements

- Windows 10/11 (x64)
- Visual Studio Build Tools / VS 2022 with C++ workload
- Windows SDK 10.0.26100+
- CMake 3.20+

## Build & Run

```powershell
# Configure
cmake -B build -G "Visual Studio 18 2026" -A x64

# Build (Debug)
cmake --build build --config Debug

# Run tests
ctest --test-dir build -C Debug --output-on-failure

# Run app
.\build\Debug\ScreenRecorder.exe
```

## Package (ZIP)

```powershell
# Configure once (if not already configured)
cmake -B build -G "Visual Studio 18 2026" -A x64

# Build release binary
cmake --build build --config Release

# Create distributable zip
cpack --config build\CPackConfig.cmake -C Release
```

Generated artifact:

- `ScreenRecorder-0.3.6-windows-x64.zip`

## Release

Use GitHub CLI to publish a tagged release with the package:

```powershell
git tag v0.3.6
git push origin v0.3.6
gh release create v0.3.6 ScreenRecorder-0.3.6-windows-x64.zip --title "v0.3.6" --notes "Release v0.3.6: stable hardware-first recording, efficiency/HQ profiles, camera overlay tuning, recording diagnostics, and app icon polish."
```

## Project Layout

```text
src/
  app/ controller/ capture/ audio/ encoder/ storage/ sync/ utils/
tests/
  unit/ integration/
```

## CI

GitHub Actions workflow: `.github/workflows/windows-ci.yml`

## Notes

- Use Visual Studio generator for reliable Windows builds.
- Each completed recording has a matching `.diagnostics.txt` file beside the MP4.
- If finalize fails, output remains `.partial.mp4` and is not renamed to `.mp4`.

## License

MIT License. See `LICENSE`.
