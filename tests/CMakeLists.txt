include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)

# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Collect all unit test sources
file(GLOB_RECURSE UNIT_TEST_SRC unit/*.cpp)

# Collect project source files that the tests need (exclude main.cpp / WinMain)
file(GLOB_RECURSE UTIL_SRC    ${CMAKE_SOURCE_DIR}/src/utils/*.cpp)
file(GLOB_RECURSE CTRL_SRC    ${CMAKE_SOURCE_DIR}/src/controller/*.cpp)
file(GLOB_RECURSE SYNC_SRC    ${CMAKE_SOURCE_DIR}/src/sync/*.cpp)
file(GLOB_RECURSE STORAGE_SRC ${CMAKE_SOURCE_DIR}/src/storage/*.cpp)
file(GLOB_RECURSE ENCODER_SRC ${CMAKE_SOURCE_DIR}/src/encoder/*.cpp)
file(GLOB_RECURSE AUDIO_SRC   ${CMAKE_SOURCE_DIR}/src/audio/*.cpp)
file(GLOB_RECURSE CAPTURE_SRC ${CMAKE_SOURCE_DIR}/src/capture/*.cpp)

# Filter out WinMain/main entry points
list(FILTER UTIL_SRC EXCLUDE REGEX ".*main\\.cpp$")

if(UNIT_TEST_SRC)
    add_executable(unit_tests ${UNIT_TEST_SRC}
        ${UTIL_SRC} ${CTRL_SRC} ${SYNC_SRC} ${STORAGE_SRC}
        ${ENCODER_SRC} ${AUDIO_SRC} ${CAPTURE_SRC}
    )
    target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(unit_tests PRIVATE
        GTest::gtest_main
        d3d11 dxgi dxguid mfplat mfuuid mfreadwrite mf
        wmcodecdspuuid ole32 windowsapp Shlwapi Propsys
    )
    target_compile_definitions(unit_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINRT_LEAN_AND_MEAN
    )

    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif()

# Integration tests (placeholder)
file(GLOB_RECURSE INTEGRATION_TEST_SRC integration/*.cpp)
if(INTEGRATION_TEST_SRC)
    add_executable(integration_tests ${INTEGRATION_TEST_SRC}
        ${UTIL_SRC} ${CTRL_SRC} ${SYNC_SRC} ${STORAGE_SRC}
        ${ENCODER_SRC} ${AUDIO_SRC} ${CAPTURE_SRC}
    )
    target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(integration_tests PRIVATE
        GTest::gtest_main
        d3d11 dxgi dxguid mfplat mfuuid mfreadwrite mf
        wmcodecdspuuid ole32 windowsapp Shlwapi Propsys
    )
    target_compile_definitions(integration_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINRT_LEAN_AND_MEAN
    )
    include(GoogleTest)
    gtest_discover_tests(integration_tests)
endif()
