cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

# Keep a development build's default installation inside its build directory.
if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${PROJECT_BINARY_DIR}/install" CACHE PATH
        "Installation directory" FORCE)
endif()

option(BUILD_SHARED "Build first-party libraries as shared libraries" OFF)
option(BUILD_FRAME_CONVTER "Build the optional OpenCV frame conversion library" OFF)
option(BUILD_TESTS "Build tests for refactored modules" ${PROJECT_IS_TOP_LEVEL})
set(BUILD_SHARED_LIBS OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>"
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVFORMAT AVUTIL AVFILTER AVDEVICE SWRESAMPLE)
find_package(Threads REQUIRED)

# Add options after dependencies so only first-party C++ targets inherit them.
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/wd4251;/wd4275;/permissive-;/Zc:__cplusplus>"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MW_CXX_WARNING_OPTIONS
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wformat=2
        -Wundef
        -Wcast-qual
        -Wcast-align
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wold-style-cast
        -Wnull-dereference
        -Wdouble-promotion
    )
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${MW_CXX_WARNING_OPTIONS}>")
endif()

add_subdirectory(src)
include(cmake/Install.cmake)

if(BUILD_TESTS)
    enable_testing()
    set(MW_STREAMER_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/tests/data")
    add_subdirectory(tests/log)
    add_subdirectory(tests/init)
    add_subdirectory(tests/av_input)
    add_subdirectory(tests/decoder)
    add_subdirectory(tests/media_source)
    add_subdirectory(tests/media_filter)
    add_subdirectory(tests/force_fps)
    add_subdirectory(tests/audio_resampler)
    add_subdirectory(tests/syncer)
    add_subdirectory(tests/scheduler)
    add_subdirectory(tests/pipeline)
    add_subdirectory(tests/platform)
    add_subdirectory(tests/encoder)
    add_subdirectory(tests/performance)
    add_subdirectory(tests/remuxer)
    add_subdirectory(tests/recorder)
    if(BUILD_FRAME_CONVTER)
        add_subdirectory(tests/frame_convter)
    endif()
    add_subdirectory(tests/install)
endif()
