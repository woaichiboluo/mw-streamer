cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTS "Build mw-streamer tests" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_EXAMPLES "Build mw-streamer examples" OFF)
option(BUILD_OPENCV_ADAPTER "Build the optional OpenCV adapter" OFF)
option(BUILD_SHARED "Build mw-streamer as a shared library" OFF)
set(OPENSSL_ROOT "" CACHE PATH "OpenSSL installation root")
set(BUILD_SHARED_LIBS OFF)

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

find_package(FFmpeg REQUIRED COMPONENTS
    AVCODEC
    AVFORMAT
    AVUTIL
    SWRESAMPLE
    SWSCALE
)
find_package(SRT REQUIRED)
if(OPENSSL_ROOT)
    set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT}")
endif()
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

add_subdirectory(third_party/ZLMediaKit)
add_subdirectory(third_party/HdrHistogram_c)
add_subdirectory(src)

if(BUILD_OPENCV_ADAPTER)
    add_subdirectory(adapters/opencv)
endif()

if(BUILD_TESTS)
    enable_testing()

    add_library(mw::streamer_test ALIAS mw_streamer)

    add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
