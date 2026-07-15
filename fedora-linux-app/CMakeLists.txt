cmake_minimum_required(VERSION 3.20)
project(FedoraScreenRecorder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(PORTAL REQUIRED IMPORTED_TARGET libportal libportal-gtk4)

add_executable(fedora-screen-recorder src/main.cpp)
target_link_libraries(fedora-screen-recorder PRIVATE
    PkgConfig::GTK4
    PkgConfig::ADWAITA
    PkgConfig::GSTREAMER
    PkgConfig::PORTAL
)
target_compile_options(fedora-screen-recorder PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(fedora-profile-tests tests/profile_policy_test.cpp)
    add_executable(fedora-camera-device-tests tests/camera_devices_test.cpp)
    target_include_directories(fedora-profile-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(fedora-camera-device-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fedora-profile-tests PRIVATE GTest::gtest_main)
    target_link_libraries(fedora-camera-device-tests PRIVATE GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(fedora-profile-tests)
    gtest_discover_tests(fedora-camera-device-tests)
endif()

include(GNUInstallDirs)
install(TARGETS fedora-screen-recorder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES io.github.screenrecorder.Fedora.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES io.github.screenrecorder.Fedora.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES io.github.screenrecorder.Fedora.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
