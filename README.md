# ScreenRecorder

Lightweight native Windows screen recorder built with C++20, Win32, Windows Graphics Capture, Media Foundation, D3D11, Intel Quick Sync capable hardware encoding, and WASAPI.

> Fedora 44 GNOME/Wayland users: a separate, portal-first native implementation lives in [fedora-linux-app](fedora-linux-app/README.md). It uses PipeWire + GStreamer and does not attempt to build the Windows source tree on Linux.

![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green)
![Status](https://img.shields.io/badge/Status-Stable-green)

Website: [screen-recorder-windows.netlify.app](https://screen-recorder-windows.netlify.app)

## Download

The latest Windows x64 release is [Screen Recorder v0.3.16](https://github.com/priyanshuchawda/screen-recorder/releases/tag/v0.3.16).
Download [ScreenRecorder-0.3.16-windows-x64.zip](https://github.com/priyanshuchawda/screen-recorder/releases/download/v0.3.16/ScreenRecorder-0.3.16-windows-x64.zip), extract it, and run `ScreenRecorder.exe`.

## Features

- Full-screen capture via Windows Graphics Capture (WGC)
- Hardware-first H.264 video + AAC audio muxed to MP4 using Media Foundation
- Intel GPU / Quick Sync preference when available, with graceful software fallback
- **System Audio Capture**: Record desktop/YouTube audio via WASAPI Loopback
- **Audio synchronization**: Microphone and desktop audio are aligned by capture timestamps before mixing
- **Microphone Auto-Level**: Smooth automatic gain with a soft peak limiter; no hard noise gate
- **Camera Overlay**: Throttled efficient default preview with an HQ 720p-capable profile
- **Anti-Ducking**: Opt-out from Windows auto-lowering volume during capture
- **Resolution choices**: 360p, 480p, 720p, and 1080p output profiles with frame-rate-aware bitrates
- **Video continuity**: Keeps the newest frame under encoder pressure, preserves source timestamps, and falls back if hardware encoding stops producing frames
- **Recording Diagnostics**: Timestamped per-session logs in `_diagnostics` record encoder/capture settings, 30-second pipeline heartbeats, errors, WGC callback stage, and each stop/finalize stage
- Embedded app icon for the main and settings windows
- Pause/resume with monotonic timestamp rebasing
- Mute/unmute with silence injection
- Recovery flow for orphaned `.partial.mp4` files
- Low-disk auto-stop and output directory management

## Audio behavior

- Captures the Windows default microphone and desktop output through WASAPI.
- Aligns both sources by capture timestamps, then mixes them into one stereo track.
- Writes 48 kHz AAC-LC audio at 192 kbps. Desktop audio is mixed below the microphone to keep speech foregrounded.
- Microphone auto-leveling and a soft limiter help control level changes and peaks. The recorder does not currently include voice noise suppression or a compressor, and the MP4 does not contain separate microphone and desktop tracks.

Because the sources are combined in the recording, you cannot change their relative levels afterward in a standard player. For clearer speech, use a close microphone and set its input level in Windows before recording.

## Performance Profiles

- **Resolution presets** use 640x360, 848x480, 1280x720, or 1920x1080 output targets. The main-window resolution button cycles through presets; Settings offers the full selector.
- On battery, 360p and 480p use the existing power-saving encoder caps. 720p and 1080p preserve the selected quality profile, which can use more battery and storage.
- **Camera preview** intentionally uses a throttled RGB32/GDI overlay path today. It avoids adding a second GPU composition pipeline, keeps fallback simple across webcams, and is rate-limited to reduce CPU/battery cost. Screen capture and video encoding still use the D3D11/Media Foundation hardware path when available.
- Every recording writes a timestamped diagnostics sidecar to `<recording folder>\_diagnostics\<recording name>.diagnostics.txt`. It includes settings, app log messages, and a low-frequency pipeline heartbeat (frame/audio counters, queue depth, output file size, encoder stage, and WGC frame-callback stage/age). On stop, each shutdown stage records begin and completion, including WGC event revoke, frame-pool close, and capture-session close, so a later hang identifies the last operation reached. Heartbeats are written every 30 seconds to keep logging overhead low.

## Build Requirements

- Windows 10/11 (x64)
- Visual Studio 2026 (Visual Studio 18) or Build Tools 18 with the C++ workload
- Windows SDK 10.0.26100 or later
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

- `ScreenRecorder-0.3.16-windows-x64.zip`

## Release

Use GitHub CLI to publish a tagged release with the package:

```powershell
git tag v0.3.16
git push origin v0.3.16
gh release create v0.3.16 ScreenRecorder-0.3.16-windows-x64.zip --title "Screen Recorder v0.3.16" --notes "Improves recording output progress and stall diagnostics."
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

- The supported build commands use the Visual Studio 18 2026 generator.
- Each recording writes its `.diagnostics.txt` sidecar directly into `_diagnostics`, alongside the upload watcher's own `watcher.log` under `_uploader`.
- Diagnostics identify the selected adapter, encoder mode, power, profile, queue drops, output file size, and low-frequency audio/video progress.
- Stop diagnostics log the begin/completion state for capture stop, audio stop, encoder-thread join, encoder flush, and MP4 finalization. A missing completion entry identifies where shutdown stopped progressing. Hardware output stalls trigger a software fallback after five seconds.
- If finalize fails, output remains `.partial.mp4` and is not renamed to `.mp4`.

## License

MIT License. See `LICENSE`.
