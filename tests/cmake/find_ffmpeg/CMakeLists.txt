cmake_minimum_required(VERSION 3.22)

project(find_ffmpeg_fixture LANGUAGES NONE)

list(PREPEND CMAKE_MODULE_PATH "${FIND_FFMPEG_MODULE_DIR}")
find_package(FFmpeg REQUIRED COMPONENTS AVCODEC)

get_target_property(_target_type FFmpeg::avcodec TYPE)
if(NOT _target_type STREQUAL EXPECTED_TARGET_TYPE)
    message(FATAL_ERROR
        "Expected FFmpeg::avcodec type ${EXPECTED_TARGET_TYPE}, "
        "got ${_target_type}"
    )
endif()

get_target_property(_aggregate_libraries
    FFmpeg::FFmpeg INTERFACE_LINK_LIBRARIES
)
if(NOT "FFmpeg::avcodec" IN_LIST _aggregate_libraries)
    message(FATAL_ERROR "FFmpeg aggregate target is incomplete")
endif()

if(CASE STREQUAL "linux_shared")
    get_target_property(_location FFmpeg::avcodec IMPORTED_LOCATION)
    if(NOT _location STREQUAL "${FFmpeg_ROOT}/lib/libavcodec.so")
        message(FATAL_ERROR "Unexpected avcodec location: ${_location}")
    endif()
elseif(CASE STREQUAL "linux_static" OR CASE STREQUAL "windows_static")
    get_target_property(_libraries
        FFmpeg::avcodec INTERFACE_LINK_LIBRARIES
    )
    if(NOT "private_dependency" IN_LIST _libraries)
        message(FATAL_ERROR
            "Static FFmpeg dependencies were not propagated: ${_libraries}"
        )
    endif()
    if(CASE STREQUAL "linux_static")
        set(_expected_avutil "${FFmpeg_ROOT}/lib/libavutil.a")
    else()
        set(_expected_avutil "${FFmpeg_ROOT}/lib/avutil.lib")
    endif()
    if(NOT "${_expected_avutil}" IN_LIST _libraries)
        message(FATAL_ERROR
            "Static FFmpeg component dependency is not strict: ${_libraries}"
        )
    endif()
elseif(CASE STREQUAL "windows_shared")
    get_target_property(_implib FFmpeg::avcodec IMPORTED_IMPLIB_RELEASE)
    get_target_property(_location FFmpeg::avcodec IMPORTED_LOCATION_RELEASE)
    if(NOT _implib STREQUAL "${FFmpeg_ROOT}/lib/avcodec.lib")
        message(FATAL_ERROR "Unexpected avcodec import library: ${_implib}")
    endif()
    if(NOT _location STREQUAL "${FFmpeg_ROOT}/bin/avcodec-61.dll")
        message(FATAL_ERROR "Unexpected avcodec DLL: ${_location}")
    endif()
endif()
