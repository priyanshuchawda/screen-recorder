@echo off
REM build.bat — Configure and build ScreenRecorder
REM Uses Visual Studio 2026 generator (handles SDK paths automatically)

if "%1"=="configure" goto :do_configure
if "%1"=="build" goto :do_build
if "%1"=="test" goto :do_test
if "%1"=="release" goto :do_release
goto :do_all

:do_configure
echo === Configuring CMake (VS 2026, x64) ===
if exist build rmdir /s /q build
cmake -B build -G "Visual Studio 18 2026" -A x64
exit /b %ERRORLEVEL%

:do_build
echo === Building (Debug) ===
cmake --build build --config Debug
exit /b %ERRORLEVEL%

:do_test
echo === Running Tests ===
ctest --test-dir build --config Debug --output-on-failure
exit /b %ERRORLEVEL%

:do_release
echo === Release Build ===
cmake --build build --config Release
exit /b %ERRORLEVEL%

:do_all
echo === Full Build ===
if exist build rmdir /s /q build
cmake -B build -G "Visual Studio 18 2026" -A x64
if %ERRORLEVEL% neq 0 (
    echo CMake configure failed!
    exit /b 1
)
cmake --build build --config Debug
exit /b %ERRORLEVEL%
