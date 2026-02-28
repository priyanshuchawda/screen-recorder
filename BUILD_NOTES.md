# Build Notes & Known Issues

> Quick reference for building this project. For full details see `specs/001-screen-recorder/quickstart.md`.

## ⚠️ CMake Generator: Use Visual Studio, NOT Ninja

Ninja generator fails on this system due to `rc.exe` (manifest.rc) path resolution issues during CMake's ABI detection. **Always use the Visual Studio generator:**

```powershell
# ✅ CORRECT
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug

# ❌ BROKEN — manifest.rc failure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

## Build Commands

```powershell
# Configure (first time)
cmake -B build -G "Visual Studio 18 2026" -A x64

# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release

# Run tests
.\build\tests\Debug\unit_tests.exe

# Or via CTest
ctest --test-dir build --config Debug --output-on-failure
```

## Output Paths (VS Generator)

| Binary | Path |
|--------|------|
| App (Debug) | `build\Debug\ScreenRecorder.exe` |
| App (Release) | `build\Release\ScreenRecorder.exe` |
| Unit Tests | `build\tests\Debug\unit_tests.exe` |

## Google Test

- Uses `FetchContent` (auto-downloads v1.15.2)
- `gtest_force_shared_crt ON` is mandatory for MSVC
- First build takes ~20s for GTest compilation
