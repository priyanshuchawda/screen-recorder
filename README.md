# ScreenRecorder

Lightweight native Windows screen recorder built with C++20, Win32, Windows Graphics Capture, Media Foundation, and WASAPI.

![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green)
![Status](https://img.shields.io/badge/Status-Active%20Development-green)
![Tests](https://img.shields.io/badge/Tests-111%20passing-brightgreen)

## Features

- Full-screen capture via Windows Graphics Capture (WGC)
- H.264 video + AAC audio muxed to MP4 using Media Foundation
- Pause/resume with monotonic timestamp rebasing
- Mute/unmute with silence injection
- Recovery flow for orphaned `.partial.mp4` files
- Low-disk auto-stop and output directory management
- Software-first encoder fallback chain for stability on varied hardware

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

- `build\ScreenRecorder-0.2.1-windows-x64.zip`

## Release

Use GitHub CLI to publish a tagged release with the package:

```powershell
git tag v0.2.1
git push origin v0.2.1
gh release create v0.2.1 build\ScreenRecorder-0.2.1-windows-x64.zip --title "v0.2.1" --notes "Stable Windows package release."
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
- If finalize fails, output remains `.partial.mp4` and is not renamed to `.mp4`.

## License

MIT License. See `LICENSE`.
