cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/utf-8)
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
if(OPENSSL_ROOT)
    set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT}")
endif()
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit EXCLUDE_FROM_ALL)
add_subdirectory(third_party/HdrHistogram_c EXCLUDE_FROM_ALL)
add_subdirectory(src)

if(BUILD_TESTS)
    enable_testing()

    add_library(mw::streamer_test ALIAS mw_streamer)
    add_library(mw::log_test ALIAS mw_log)

    add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS mw_log mw_streamer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(TARGET mw_opencv_adapter)
    install(TARGETS mw_opencv_adapter
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# fmt is part of the public C++ logging interface. Keep it private to the
# package namespace while making the installed mw::log target self-contained.
set_property(TARGET fmt PROPERTY PUBLIC_HEADER)
install(TARGETS fmt
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/mw_streamer
)
install(DIRECTORY "${fmt_SOURCE_DIR}/include/fmt"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# A static mw_streamer archive cannot absorb the object code of its private
# static dependencies. Install those archives and let the package config expose
# them only through mw::streamer's transitive link interface.
if(NOT BUILD_SHARED)
    install(TARGETS
        spdlog
        hdr_histogram
        ZLToolKit
        zlmediakit
        ext-codec
        zlmediakit_srt_adapter
        flv
        mov
        mpeg
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/mw_streamer
    )
endif()

set(MW_STREAMER_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/mw_streamer"
)
set(MW_STREAMER_PACKAGE_IS_SHARED ${BUILD_SHARED})
set(MW_STREAMER_PACKAGE_HAS_OPENCV_ADAPTER ${BUILD_OPENCV_ADAPTER})

configure_package_config_file(
    cmake/mw_streamerConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/mw_streamerConfig.cmake"
    INSTALL_DESTINATION "${MW_STREAMER_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mw_streamerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mw_streamerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mw_streamerConfigVersion.cmake"
    DESTINATION "${MW_STREAMER_INSTALL_CMAKEDIR}"
)
install(FILES
    cmake/FindFFmpeg.cmake
    cmake/FindSRT.cmake
    DESTINATION "${MW_STREAMER_INSTALL_CMAKEDIR}/modules"
)

install(FILES
    include/mw/export.h
    include/mw/log.h
    include/mw/streamer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw
)

install(FILES
    include/mw/streamer/api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw/streamer
)

install(FILES
    include/mw/streamer/media/types.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw/streamer/media
)

install(FILES
    include/mw/streamer/processor/processor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw/streamer/processor
)

if(TARGET mw_opencv_adapter)
    install(FILES
        include/mw/opencv_adapter.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw
    )

    install(FILES
        include/mw/opencv_adapter/cuda_frame.h
        include/mw/opencv_adapter/cuda_mat_adapter.h
        include/mw/opencv_adapter/host_frame.h
        include/mw/opencv_adapter/host_mat_adapter.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw/opencv_adapter
    )
endif()
