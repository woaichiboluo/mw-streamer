cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_SHARED "Build logging and streamer as shared libraries" OFF)
option(BUILD_TESTS "Build adapter tests" ${PROJECT_IS_TOP_LEVEL})
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

find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVUTIL)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit EXCLUDE_FROM_ALL)
add_subdirectory(src)
target_link_libraries(ZLToolKit PUBLIC mw::log)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
