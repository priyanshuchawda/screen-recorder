cmake_minimum_required(VERSION 3.20)
project(ScreenRecorder LANGUAGES CXX)

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
    Propsys
    avrt
)

# Collect all source files by module
file(GLOB_RECURSE UTIL_SRC    src/utils/*.cpp src/utils/*.h)
file(GLOB_RECURSE CTRL_SRC    src/controller/*.cpp src/controller/*.h)
file(GLOB_RECURSE CAPTURE_SRC src/capture/*.cpp src/capture/*.h)
file(GLOB_RECURSE AUDIO_SRC   src/audio/*.cpp src/audio/*.h)
file(GLOB_RECURSE ENCODER_SRC src/encoder/*.cpp src/encoder/*.h)
file(GLOB_RECURSE SYNC_SRC    src/sync/*.cpp src/sync/*.h)
file(GLOB_RECURSE STORAGE_SRC src/storage/*.cpp src/storage/*.h)
file(GLOB_RECURSE APP_SRC     src/app/*.cpp src/app/*.h)

add_executable(ScreenRecorder WIN32
    ${APP_SRC}
    ${CTRL_SRC}
    ${CAPTURE_SRC}
    ${AUDIO_SRC}
    ${ENCODER_SRC}
    ${SYNC_SRC}
    ${STORAGE_SRC}
    ${UTIL_SRC}
)

target_include_directories(ScreenRecorder PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ScreenRecorder PRIVATE ${WIN_LIBS})

# Tests
enable_testing()
add_subdirectory(tests)

# Packaging
set(CPACK_PACKAGE_NAME "ScreenRecorder")
set(CPACK_PACKAGE_VENDOR "ScreenRecorder")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows screen recorder")
set(CPACK_GENERATOR "ZIP")
include(CPack)
