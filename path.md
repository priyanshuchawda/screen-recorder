# Windows Toolchain Notes

This project uses CMake with the Visual Studio 18 2026 x64 generator. A C++ workload and Windows SDK 10.0.26100 or later are required.

```powershell
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

See [README.md](README.md) for requirements, Debug and test commands, packaging steps, and current release information. Build paths and SDK locations are machine-specific; keep them in local environment configuration rather than this repository.
