cmake_minimum_required(VERSION 3.16)
project(adaptive_video_transport VERSION 1.0.0 LANGUAGES CXX)

option(BUILD_VIDEO_APPS "Build V4L2/FFmpeg sender and receiver" ON)
option(BUILD_TESTING "Build unit tests" ON)
option(BUILD_SENDER "Build the embedded video sender" ON)
option(BUILD_RECEIVER "Build the PC video receiver" ON)
option(BUILD_TOOLS "Build controller simulation tools" ON)
option(REQUIRE_VIDEO_APPS "Fail configure when FFmpeg video apps cannot be built" OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()
# GCC 4.9 reports aggregate value-initialization (T{}) as if fields were left
# uninitialized. They are zero-initialized by the language; suppress only that
# legacy false positive while retaining the warning on current toolchains.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5)
    add_compile_options(-Wno-missing-field-initializers)
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)
if(BUILD_TESTING)
    enable_testing()
endif()

add_library(adaptive_core
    src/controller.cpp
    src/protocol.cpp
    src/socket.cpp)
target_include_directories(adaptive_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(adaptive_core PUBLIC Threads::Threads)

if(BUILD_TOOLS)
    add_executable(controller_sim src/apps/controller_sim.cpp)
    target_link_libraries(controller_sim PRIVATE adaptive_core)
endif()

if(BUILD_VIDEO_APPS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavcodec libavformat libavutil libswscale)
    endif()
    if(FFMPEG_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(ZLIB REQUIRED)
        add_library(adaptive_media
            src/media/ffmpeg_codec.cpp
            src/media/ts_recorder.cpp
            src/media/v4l2_capture.cpp)
        target_include_directories(adaptive_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(adaptive_media PUBLIC adaptive_core PkgConfig::FFMPEG ZLIB::ZLIB)

        if(BUILD_SENDER)
            add_executable(adaptive_sender src/apps/adaptive_sender.cpp)
            target_link_libraries(adaptive_sender PRIVATE adaptive_media)
            install(TARGETS adaptive_sender RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
        if(BUILD_RECEIVER)
            add_executable(adaptive_receiver src/apps/adaptive_receiver.cpp)
            target_link_libraries(adaptive_receiver PRIVATE adaptive_media)
            install(TARGETS adaptive_receiver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
        if(BUILD_TESTING)
            add_executable(media_pipeline_test tests/media_pipeline_test.cpp)
            target_link_libraries(media_pipeline_test PRIVATE adaptive_media)
            add_test(NAME media_pipeline_test COMMAND media_pipeline_test)

            add_executable(fullhd_pipeline_test tests/fullhd_pipeline_test.cpp)
            target_link_libraries(fullhd_pipeline_test PRIVATE adaptive_media)
            add_test(NAME fullhd_pipeline_test COMMAND fullhd_pipeline_test)
        endif()
    else()
        if(REQUIRE_VIDEO_APPS)
            message(FATAL_ERROR "FFmpeg development packages or Linux V4L2 support were not found")
        else()
            message(WARNING "FFmpeg development packages were not found; video apps are disabled. Core controller, protocol, simulator and tests are still built.")
        endif()
    endif()
endif()

if(BUILD_TOOLS)
    install(TARGETS controller_sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE adaptive_core)
    add_test(NAME protocol_test COMMAND protocol_test)

    add_executable(controller_test tests/controller_test.cpp)
    target_link_libraries(controller_test PRIVATE adaptive_core)
    add_test(NAME controller_test COMMAND controller_test)

    add_executable(queue_test tests/queue_test.cpp)
    target_link_libraries(queue_test PRIVATE adaptive_core)
    add_test(NAME queue_test COMMAND queue_test)
endif()
