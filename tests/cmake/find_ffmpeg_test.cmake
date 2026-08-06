if(NOT DEFINED CASE OR NOT DEFINED CMAKE_EXECUTABLE OR
   NOT DEFINED FIND_FFMPEG_MODULE_DIR OR NOT DEFINED FIXTURE_SOURCE_DIR OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "FindFFmpeg test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
set(_package_root "${TEST_ROOT}/package")
set(_build_dir "${TEST_ROOT}/build")
file(MAKE_DIRECTORY
    "${_package_root}/include/libavcodec"
    "${_package_root}/lib/pkgconfig"
)
file(WRITE "${_package_root}/include/libavcodec/avcodec.h" "")

set(_linkage)
set(_system_arguments)
set(_expected_target_type)
set(_write_pkg_config TRUE)
if(CASE STREQUAL "linkage_required")
    set(_write_pkg_config FALSE)
elseif(CASE STREQUAL "pkg_config_required")
    set(_linkage SHARED)
    set(_write_pkg_config FALSE)
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "linux_shared")
    set(_linkage SHARED)
    set(_expected_target_type SHARED_LIBRARY)
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "linux_static")
    set(_linkage STATIC)
    set(_expected_target_type STATIC_LIBRARY)
    file(WRITE "${_package_root}/lib/libavcodec.a" "")
    file(WRITE "${_package_root}/lib/libavutil.a" "")
elseif(CASE STREQUAL "windows_shared")
    set(_linkage SHARED)
    set(_expected_target_type SHARED_LIBRARY)
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(MAKE_DIRECTORY "${_package_root}/bin")
    file(WRITE "${_package_root}/lib/avcodec.lib" "")
    file(WRITE "${_package_root}/bin/avcodec-61.dll" "")
elseif(CASE STREQUAL "windows_static")
    set(_linkage STATIC)
    set(_expected_target_type STATIC_LIBRARY)
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(WRITE "${_package_root}/lib/avcodec.lib" "")
    file(WRITE "${_package_root}/lib/avutil.lib" "")
else()
    message(FATAL_ERROR "Unknown FindFFmpeg test case: ${CASE}")
endif()

if(_write_pkg_config)
    file(WRITE "${_package_root}/lib/pkgconfig/libavcodec.pc"
        "prefix=${_package_root}\n"
        "libdir=\${prefix}/lib\n"
        "includedir=\${prefix}/include\n"
        "Name: libavcodec\n"
        "Description: FindFFmpeg test fixture\n"
        "Version: 61.19.100\n"
        "Libs: -L\${libdir} -lavcodec\n"
        "Libs.private: -lavutil -lprivate_dependency\n"
        "Cflags: -I\${includedir}\n"
    )
endif()

set(_command
    "${CMAKE_EXECUTABLE}"
    -E env
    "PKG_CONFIG_PATH="
    "PKG_CONFIG_LIBDIR=${_package_root}/lib/pkgconfig"
    "${CMAKE_EXECUTABLE}"
    -S "${FIXTURE_SOURCE_DIR}"
    -B "${_build_dir}"
    "-DCASE=${CASE}"
    "-DEXPECTED_TARGET_TYPE=${_expected_target_type}"
    "-DFIND_FFMPEG_MODULE_DIR=${FIND_FFMPEG_MODULE_DIR}"
    "-DFFmpeg_ROOT=${_package_root}"
    ${_system_arguments}
)
if(_linkage)
    list(APPEND _command "-DFFMPEG_LINKAGE=${_linkage}")
endif()

execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

if(CASE STREQUAL "linkage_required")
    if(_result EQUAL 0 OR
       NOT "${_stdout}${_stderr}" MATCHES
           "FFMPEG_LINKAGE must be explicitly set to STATIC or SHARED")
        message(FATAL_ERROR
            "Missing FFMPEG_LINKAGE was not rejected as expected:\n"
            "${_stdout}${_stderr}"
        )
    endif()
elseif(CASE STREQUAL "pkg_config_required")
    if(_result EQUAL 0 OR
       NOT "${_stdout}${_stderr}" MATCHES "Could NOT find FFmpeg" OR
       NOT "${_stdout}${_stderr}" MATCHES "AVCODEC")
        message(FATAL_ERROR
            "Missing FFmpeg pkg-config metadata was not rejected:\n"
            "${_stdout}${_stderr}"
        )
    endif()
elseif(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "FindFFmpeg ${CASE} fixture failed:\n${_stdout}${_stderr}"
    )
endif()
