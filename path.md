# Native Windows C++ Setup Paths

> **Related**: See also [specs/001-screen-recorder/quickstart.md](./specs/001-screen-recorder/quickstart.md) for build instructions using CMake + Ninja.

This document serves as a record of the critical system paths, SDK versions, and toolchains required to compile Native Windows C++ projects (specifically for C++/WinRT, Direct3D, and Media Foundation) on this machine without relying on the full Visual Studio IDE. 

Because the standard `vcvars64.bat` script sometimes fails to map paths perfectly in isolated environments, these exact paths are injected manually in `build.bat` to guarantee a successful compile.

> **Note**: Both VS Build Tools AND VS Community 2026 are installed on this machine:
> - Build Tools: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` (used by `build.bat`)
> - Community: `C:\Program Files\Microsoft Visual Studio\18\Community` (used by CMake/Ninja)
> - Both contain the same MSVC toolset `14.50.35717` with identical `cl.exe`.

---

### 1. MSVC Compiler (Build Tools)
The raw Microsoft Visual C++ compiler (`cl.exe`) capable of building modern C++20.
- **Base Version Directory**: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.50.35717`
- **Compiler Binary (`cl.exe`) Path**: `...\bin\Hostx64\x64`
- **Include Directory (C++ Standard Library)**: `...\include`
- **Library Directory (`lib`)**: `...\lib\x64`

### 2. Windows 10 SDK
Provides all the native Windows APIs, headers, and libraries required to hook into hardware capture, COM, and WASAPI audio. 
- **Base Directory**: `C:\Program Files (x86)\Windows Kits\10`
- **Installed Version target**: `10.0.26100.0`
- **Binary Utilities (`rc.exe`, `mt.exe`, `cppwinrt.exe`)**: `...\bin\10.0.26100.0\x64`

#### SDK Include Directories required for WinRT & Media Foundation:
1. `...\Include\10.0.26100.0\shared` (Fundamental Windows Types)
2. `...\Include\10.0.26100.0\ucrt` (Universal C Runtime headers)
3. `...\Include\10.0.26100.0\um` (User Mode headers - e.g., Media Foundation `mfapi.h`, D3D `d3d11.h`)
4. `...\Include\10.0.26100.0\winrt` (WinRT base projection headers)
5. `...\Include\10.0.26100.0\cppwinrt` (C++/WinRT tool-generated projection headers)

#### SDK Library (`.lib`) Directories required to Link the EXE:
1. `...\Lib\10.0.26100.0\ucrt\x64` (Universal C Runtime `lib`s)
2. `...\Lib\10.0.26100.0\um\x64` (User Mode `lib`s - e.g., `kernel32.lib`, `mfplat.lib`, `d3d11.lib`)

### 3. Build Utilities
The modern cross-IDE tooling that actually interprets `CMakeLists.txt` and drives the compiler extremely fast.
- **CMake (`cmake.exe`)**: `C:\Program Files\CMake\bin`
- **Ninja (`ninja.exe`)**: `C:\Users\Admin\AppData\Local\Microsoft\WinGet\Links`

---

### How Everything is Injected (`build.bat` Logic)
When CMake runs inside our Build script, it has to be aware of:
1. The **`PATH`** environment variable (so it can locate CMake, Ninja, `cl.exe`, and `rc.exe`).
2. The **`INCLUDE`** environment variable (so the `#include <...>` headers in `main.cpp` resolve to both standard C++ and Windows SDK headers).
3. The **`LIB`** environment variable (so the linker knows where standard libraries like `kernel32.lib` reside for the `x64` architecture).
4. Explicit flags to CMake defining which binaries to use:
   - `-DCMAKE_C_COMPILER=cl.exe`
   - `-DCMAKE_CXX_COMPILER=cl.exe`
   - `-DCMAKE_RC_COMPILER=rc.exe`

As long as these paths remain valid, this project will compile natively forever.
