cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_SHARED "Build logging and streamer as shared libraries" OFF)
option(BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
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
    add_compile_options(/utf-8)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(cmake/Dependencies.cmake)

find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVFORMAT AVUTIL SWRESAMPLE)
if(AVFORMAT_VERSION VERSION_LESS 62)
    message(FATAL_ERROR
        "FFmpeg 8.0 or newer (libavformat >= 62) is required for legacy HEVC RTMP CodecID 12 support")
endif()
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit EXCLUDE_FROM_ALL)

# Keep dependency headers outside the first-party warning policy.
foreach(dependency IN ITEMS zlmediakit ZLToolKit ext-codec)
    get_target_property(dependency_include_dirs ${dependency} INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET ${dependency} APPEND PROPERTY
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dependency_include_dirs}")
endforeach()

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
target_link_libraries(ZLToolKit PUBLIC mw::log)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
