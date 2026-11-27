include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)

# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Collect all unit test sources
file(GLOB_RECURSE UNIT_TEST_SRC CONFIGURE_DEPENDS unit/*.cpp)

if(UNIT_TEST_SRC)
    add_executable(unit_tests ${UNIT_TEST_SRC})
    target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(unit_tests PRIVATE
        GTest::gtest_main
        screen_recorder_core
    )
    target_compile_definitions(unit_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINRT_LEAN_AND_MEAN
    )

    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif()

# Integration tests (placeholder)
file(GLOB_RECURSE INTEGRATION_TEST_SRC CONFIGURE_DEPENDS integration/*.cpp)
if(INTEGRATION_TEST_SRC)
    add_executable(integration_tests ${INTEGRATION_TEST_SRC})
    target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(integration_tests PRIVATE
        GTest::gtest_main
        screen_recorder_core psapi
    )
    target_compile_definitions(integration_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINRT_LEAN_AND_MEAN
    )
    include(GoogleTest)
    gtest_discover_tests(integration_tests DISCOVERY_TIMEOUT 30)
endif()
