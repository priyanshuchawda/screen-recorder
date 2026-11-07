# Contributing

Thanks for contributing to ScreenRecorder.

## Development Setup

1. Install Visual Studio 2026 / Build Tools 18 with the C++ workload and Windows SDK 10.0.26100 or later.
2. Configure:
   ```powershell
   cmake -B build -G "Visual Studio 18 2026" -A x64
   ```
3. Build:
   ```powershell
   cmake --build build --config Debug
   ```
4. Test:
   ```powershell
   ctest --test-dir build -C Debug --output-on-failure
   ```

## Guidelines

- Keep changes focused and minimal.
- Add or update tests for behavioral changes.
- Prefer root-cause fixes over workarounds.
- Keep Windows API error handling explicit (log HRESULT/GetLastError).

## Pull Requests

- Use clear, imperative commit messages.
- Include validation evidence (build + tests).
- Mention any runtime caveats in the PR description.
