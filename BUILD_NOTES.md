# Windows Build Notes

The supported Windows build uses the Visual Studio 18 2026 x64 generator and CMake. See the [README build section](README.md#build--run) for prerequisites and the full build/package workflow.

```powershell
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target ScreenRecorder
cpack --config build\CPackConfig.cmake -C Release
```

The ZIP is generated in the repository root as `ScreenRecorder-<version>-windows-x64.zip`.

## Output paths

| Artifact | Path |
|---|---|
| App (Debug) | `build\Debug\ScreenRecorder.exe` |
| App (Release) | `build\Release\ScreenRecorder.exe` |
| Unit tests | `build\tests\Debug\unit_tests.exe` |

Run unit and integration tests with:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Google Test is fetched by CMake `FetchContent` during configuration.
