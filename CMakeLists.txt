cmake_minimum_required(VERSION 3.20)
project(ScreenRecorder VERSION 0.3.18 LANGUAGES CXX)

# Embed the source revision in recording diagnostics so hardware results can
# be tied to the exact candidate. Fall back cleanly for source archives.
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SCREEN_RECORDER_BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT SCREEN_RECORDER_BUILD_COMMIT)
    set(SCREEN_RECORDER_BUILD_COMMIT "unknown")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC-specific flags
if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus /EHsc)
    # Embed manifest directly instead of using rc.exe (avoids path issues)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:EMBED")
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        WINRT_LEAN_AND_MEAN
    )
endif()

# Windows SDK libs needed
set(WIN_LIBS
    d3d11
    dxgi
    dxguid
    mfplat
    mfuuid
    mfreadwrite
    mf
    wmcodecdspuuid
    ole32
    windowsapp
    Shlwapi
    shell32
    Propsys
    avrt
)

# Collect all source files by module
file(GLOB_RECURSE UTIL_SRC    CONFIGURE_DEPENDS src/utils/*.cpp src/utils/*.h)
file(GLOB_RECURSE CTRL_SRC    CONFIGURE_DEPENDS src/controller/*.cpp src/controller/*.h)
file(GLOB_RECURSE CAPTURE_SRC CONFIGURE_DEPENDS src/capture/*.cpp src/capture/*.h)
file(GLOB_RECURSE AUDIO_SRC   CONFIGURE_DEPENDS src/audio/*.cpp src/audio/*.h)
file(GLOB_RECURSE ENCODER_SRC CONFIGURE_DEPENDS src/encoder/*.cpp src/encoder/*.h)
file(GLOB_RECURSE SYNC_SRC    CONFIGURE_DEPENDS src/sync/*.cpp src/sync/*.h)
file(GLOB_RECURSE STORAGE_SRC CONFIGURE_DEPENDS src/storage/*.cpp src/storage/*.h)
file(GLOB_RECURSE APP_SRC     CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
list(APPEND APP_SRC
    src/app/camera_overlay.cpp
    src/app/camera_overlay.h
    src/app.rc
)

# Compile the production recording engine once and link it into the app/tests.
# The manual probe gets a separate copy below so test-only fault injection can
# never leak into the application binary.
set(SCREEN_RECORDER_CORE_SOURCES
    ${CTRL_SRC}
    ${CAPTURE_SRC}
    ${AUDIO_SRC}
    ${ENCODER_SRC}
    ${SYNC_SRC}
    ${STORAGE_SRC}
    ${UTIL_SRC}
)

add_library(screen_recorder_core STATIC ${SCREEN_RECORDER_CORE_SOURCES})
target_include_directories(screen_recorder_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(screen_recorder_core PRIVATE
    SR_BUILD_COMMIT=L"${SCREEN_RECORDER_BUILD_COMMIT}"
)
target_link_libraries(screen_recorder_core PUBLIC ${WIN_LIBS})

add_executable(ScreenRecorder WIN32
    ${APP_SRC}
)

target_include_directories(ScreenRecorder PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ScreenRecorder PRIVATE screen_recorder_core)

install(TARGETS ScreenRecorder
    RUNTIME DESTINATION .
)
install(FILES
    ${CMAKE_SOURCE_DIR}/README.md
    ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION .
)

# Tests
enable_testing()
add_subdirectory(tests)

# Explicitly built local hardware diagnostic; it records a disposable desktop
# session and is not run as part of the unattended unit-test suite.
add_executable(recording_probe EXCLUDE_FROM_ALL
    tests/manual/recording_probe.cpp
)
target_include_directories(recording_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(recording_probe PRIVATE SR_LOCAL_RECORDING_PROBE)

# Fault injection must be compiled into the mux writer translation unit, which
# belongs to the library. Keep this probe-only copy isolated from the production
# core so the app cannot contain or activate the simulated writer stall.
add_library(screen_recorder_probe_core STATIC EXCLUDE_FROM_ALL ${SCREEN_RECORDER_CORE_SOURCES})
target_include_directories(screen_recorder_probe_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(screen_recorder_probe_core PUBLIC ${WIN_LIBS})
target_compile_definitions(screen_recorder_probe_core PRIVATE SR_LOCAL_RECORDING_PROBE)
target_link_libraries(recording_probe PRIVATE screen_recorder_probe_core psapi)

# Packaging
set(CPACK_PACKAGE_NAME "ScreenRecorder")
set(CPACK_PACKAGE_VENDOR "ScreenRecorder")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows screen recorder")
set(CPACK_PACKAGE_FILE_NAME "ScreenRecorder-${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR "ZIP")
include(CPack)
