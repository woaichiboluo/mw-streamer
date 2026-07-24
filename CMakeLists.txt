cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

include(CTest)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(cmake/Dependencies.cmake)

find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVUTIL)
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit)

add_library(mw_streamer STATIC
    src/convter/ZlmCodecParametersConvter.cpp
    src/convter/ZlmPacketConvter.cpp
    src/init/init.cpp
    src/log/logging.cpp
)
add_library(mw::streamer ALIAS mw_streamer)

target_include_directories(mw_streamer
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)

target_link_libraries(mw_streamer
    PUBLIC
        FFmpeg::avcodec
        ZLMediaKit::MediaKit
        fmt::fmt
    PRIVATE
        FFmpeg::avutil
        SRT::SRT
        ZLMediaKit::SRT
        ZLMediaKit::ToolKit
        spdlog::spdlog
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
