# MIT License
#
# Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

cmake_minimum_required(VERSION 3.1.3...3.26)

project(ZLMediaKit LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "mw-streamer protocol kernel supports Linux and Windows only")
endif()

# 使能 C++11
# Enable C++11
set(CMAKE_CXX_STANDARD 11)
# -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_ASAN "Enable Address Sanitize" OFF)
option(ENABLE_MEM_DEBUG "Enable Memory Debug" OFF)
option(ENABLE_MSVC_MT "Enable MSVC Mt/Mtd lib" ON)
option(USE_SOLUTION_FOLDERS "Enable solution dir supported" ON)

# mw-streamer 固定能力，不对外提供裁剪开关。
set(ENABLE_HLS ON CACHE INTERNAL "mw-streamer fixed feature" FORCE)
set(ENABLE_MP4 ON CACHE INTERNAL "mw-streamer fixed feature" FORCE)
set(ENABLE_RTPPROXY ON CACHE INTERNAL "mw-streamer fixed feature" FORCE)
set(ENABLE_SRT ON CACHE INTERNAL "mw-streamer fixed feature" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "mw-streamer builds static libraries" FORCE)

##############################################################################
# 设置socket默认缓冲区大小为256k.如果设置为0则不设置socket的默认缓冲区大小,使用系统内核默认值(设置为0仅对linux有效)
# Set the default buffer size of the socket to 256k. If set to 0, the default buffer size of the socket will not be set,
# and the system kernel default value will be used (setting to 0 is only valid for linux)
set(SOCKET_DEFAULT_BUF_SIZE 262144 CACHE STRING "Default buffer size for socket" FORCE)

if("${CMAKE_BUILD_TYPE}" STREQUAL "")
  set(CMAKE_BUILD_TYPE "Debug")
endif()

message(STATUS "编译类型: ${CMAKE_BUILD_TYPE}")

set(CMAKE_INCLUDE_CURRENT_DIR OFF)

# 安装路径
# Install path
if(NOT CMAKE_INSTALL_PREFIX)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(INSTALL_PATH_LIB     lib${LIB_SUFFIX})
    set(INSTALL_PATH_INCLUDE include)
  elseif(WIN32)
    # Windows 下安装到了用户主目录下?
    # Install to the user's home directory under Windows?
    set(INSTALL_PATH_LIB     $ENV{HOME}/${CMAKE_PROJECT_NAME}/lib)
    set(INSTALL_PATH_INCLUDE $ENV{HOME}/${CMAKE_PROJECT_NAME}/include)
  else()
    message(WARNING "该平台(${CMAKE_SYSTEM_NAME})下暂未设置安装路径")
  endif()
else()
  set(INSTALL_PATH_LIB     ${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX})
  set(INSTALL_PATH_INCLUDE ${CMAKE_INSTALL_PREFIX}/include)
endif()

# 设置静态库输出目录
# Windows 也不再区分 32/64
# Set the static library output directory
# Windows no longer distinguishes 32/64
set(OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/output/${CMAKE_BUILD_TYPE})
set(LIBRARY_OUTPUT_PATH    ${OUTPUT_DIR})

# 添加 git 版本信息
# Add git version information
set(COMMIT_HASH "Git_Unkown_commit")
set(COMMIT_TIME "Git_Unkown_time")
set(BRANCH_NAME "Git_Unkown_branch")
set(BUILD_TIME "")

string(TIMESTAMP BUILD_TIME "%Y-%m-%dT%H:%M:%S")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
    OUTPUT_VARIABLE COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  execute_process(
    COMMAND ${GIT_EXECUTABLE} symbolic-ref --short -q HEAD
    OUTPUT_VARIABLE BRANCH_NAME
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  execute_process(
    COMMAND ${GIT_EXECUTABLE} log --format=format:%aI -1
    OUTPUT_VARIABLE COMMIT_TIME
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/ZLMVersion.h.ini
  ${CMAKE_CURRENT_BINARY_DIR}/ZLMVersion.h
  @ONLY)

message(STATUS "Git version is ${BRANCH_NAME} ${COMMIT_HASH}/${COMMIT_TIME} ${BUILD_TIME}")

##############################################################################

# 方便修改全局变量
# Convenient to modify global variables
function(update_cached name value)
  set("${name}" "${value}" CACHE INTERNAL "*** Internal ***" FORCE)
endfunction()

function(update_cached_list name)
  set(_tmp_list "${${name}}")
  list(APPEND _tmp_list "${ARGN}")
  list(REMOVE_DUPLICATES _tmp_list)
  update_cached(${name} "${_tmp_list}")
endfunction()

# TODO:
function(set_file_group prefix)
  message(STATUS "set_file_group " ${prefix} " " ${ARGC})
  foreach(FILE IN LISTS ARGN 1)
    # Get the directory of the source file
    get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

    # Remove common directory prefix to make the group
    string(REPLACE "${prefix}" "" GROUP "${PARENT_DIR}")

    # Make sure we are using windows slashes
    string(REPLACE "/" "\\" GROUP "${GROUP}")

    source_group("${GROUP}" FILES "${FILE}")
  endforeach()
endfunction()


find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK    ccache)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Linux
  set(COMPILE_OPTIONS_DEFAULT
    "-fPIC"
    "-Wall;-Wextra"
    "-Wno-unused-function;-Wno-unused-parameter;-Wno-unused-variable;-Wno-deprecated-declarations"
    "-Wno-error=extra;-Wno-error=missing-field-initializers;-Wno-error=type-limits;-Wno-comment")

  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    set(COMPILE_OPTIONS_DEFAULT ${COMPILE_OPTIONS_DEFAULT} "-g3")
  endif()
elseif(WIN32)
  if (MSVC)
    set(COMPILE_OPTIONS_DEFAULT
            # TODO: /wd4819 应该是不会生效
            "/wd4566;/wd4819;/utf-8"
            # warning C4530: C++ exception handler used, but unwind semantics are not enabled.
            "/EHsc")
    # disable Windows logo
   string(REPLACE "/nologo" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()
endif()

# mediakit 以及各个 runtime 依赖
# mediakit and various runtime dependencies
update_cached(MK_LINK_LIBRARIES "")
update_cached(MK_COMPILE_DEFINITIONS "ENABLE_VERSION;ENABLE_HLS;ENABLE_MP4;ENABLE_RTPPROXY;ENABLE_SRT")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(CheckCXXSourceCompiles)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/cmake/checks/atomic_check.cpp atomic_check_cpp)
  check_cxx_source_compiles("${atomic_check_cpp}" HAVE_CXX_ATOMICS_WITHOUT_LIB)
  if(NOT HAVE_CXX_ATOMICS_WITHOUT_LIB)
    # cmake --help-policy CMP0075
    list(APPEND CMAKE_REQUIRED_LIBRARIES "atomic")
    check_cxx_source_compiles("${atomic_check_cpp}" HAVE_CXX_ATOMICS_WITH_LIB)
    if(NOT HAVE_CXX_ATOMICS_WITH_LIB)
      message(WARNING "Compiler doesn't support std::atomic<long long>")
    else()
      update_cached_list(MK_LINK_LIBRARIES atomic)
    endif()
  endif()
endif()

if(ENABLE_MEM_DEBUG)
  update_cached_list(MK_LINK_LIBRARIES
    "-Wl,-wrap,free;-Wl,-wrap,malloc;-Wl,-wrap,realloc;-Wl,-wrap,calloc")
  update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_MEM_DEBUG)
  message(STATUS "Memory debugging enabled")
endif()

if(ENABLE_ASAN)
  list(APPEND COMPILE_OPTIONS_DEFAULT
    "-fsanitize=address;-fno-omit-frame-pointer")
  # https://github.com/google/sanitizers/wiki/AddressSanitizer#using-addresssanitizer
  # > In order to use AddressSanitizer you will need to
  # > compile and link your program using clang with the -fsanitize=address switch.
  update_cached_list(MK_LINK_LIBRARIES "-fsanitize=address")
  message(STATUS "Address Sanitize enabled")
endif()

# OpenSSL 是可选依赖；基础 RTMP/RTSP/SRT/HTTP 能力不依赖它。
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  set(ENABLE_OPENSSL ON CACHE INTERNAL "OpenSSL detected" FORCE)
  message(STATUS "found library: ${OPENSSL_LIBRARIES}, ENABLE_OPENSSL defined")
  include_directories(${OPENSSL_INCLUDE_DIR})
  update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_OPENSSL)
  update_cached_list(MK_LINK_LIBRARIES ${OPENSSL_LIBRARIES})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND OPENSSL_USE_STATIC_LIBS)
    update_cached_list(MK_LINK_LIBRARIES ${CMAKE_DL_LIBS})
  elseif(WIN32 AND OPENSSL_USE_STATIC_LIBS)
    update_cached_list(MK_LINK_LIBRARIES Crypt32)
  endif()
else()
  set(ENABLE_OPENSSL OFF CACHE INTERNAL "OpenSSL not found" FORCE)
  message(STATUS "OpenSSL not found: TLS and encrypted SRT passphrases are unavailable")
endif()

if(WIN32)
  update_cached_list(MK_LINK_LIBRARIES WS2_32 Iphlpapi shlwapi)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  update_cached_list(MK_LINK_LIBRARIES pthread)
endif()

# ----------------------------------------------------------------------------
# Solution folders:
# ----------------------------------------------------------------------------
if(USE_SOLUTION_FOLDERS)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
  set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")
endif()

if(MSVC AND ENABLE_MSVC_MT)
  set(CompilerFlags
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_CXX_FLAGS_MINSIZEREL
    CMAKE_C_FLAGS
    CMAKE_C_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE
    CMAKE_C_FLAGS_RELWITHDEBINFO
    CMAKE_C_FLAGS_MINSIZEREL)
  # TODO: 通常应该不需要替换
  foreach(CompilerFlag ${CompilerFlags})
    string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
  endforeach()
endif()

##############################################################################

# for version.h
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# for assert.h
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdpart)

add_subdirectory(3rdpart)

add_subdirectory(src)

add_subdirectory(ext-codec)

add_subdirectory(srt)
