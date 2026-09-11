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

set(_system_arguments)
set(_write_pkg_config TRUE)
set(_pkg_config_version 61.19.100)
if(CASE STREQUAL "pkg_config_required")
    set(_write_pkg_config FALSE)
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "linux_shared")
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "linux_static")
    file(WRITE "${_package_root}/lib/libavcodec.a" "")
    file(WRITE "${_package_root}/lib/libavutil.a" "")
elseif(CASE STREQUAL "linux_auto")
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
    file(WRITE "${_package_root}/lib/libavcodec.a" "")
elseif(CASE STREQUAL "root_selection")
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "old_avcodec")
    set(_pkg_config_version 58.134.100)
    file(WRITE "${_package_root}/lib/libavcodec.so" "")
elseif(CASE STREQUAL "windows_shared")
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(MAKE_DIRECTORY "${_package_root}/bin")
    file(WRITE "${_package_root}/lib/avcodec.lib" "")
    file(WRITE "${_package_root}/lib/avcodec_static.lib" "")
    file(WRITE "${_package_root}/bin/avcodec-61.dll" "")
elseif(CASE STREQUAL "windows_static")
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(WRITE "${_package_root}/lib/avcodec.lib" "")
    file(WRITE "${_package_root}/lib/avutil.lib" "")
else()
    message(FATAL_ERROR "Unknown FindFFmpeg test case: ${CASE}")
endif()

set(_pkg_config_libdir "${_package_root}/lib/pkgconfig")
if(CASE STREQUAL "root_selection")
    set(_decoy_root "${TEST_ROOT}/decoy")
    file(MAKE_DIRECTORY
        "${_decoy_root}/include/libavcodec"
        "${_decoy_root}/lib/pkgconfig"
    )
    file(WRITE "${_decoy_root}/include/libavcodec/avcodec.h" "")
    file(WRITE "${_decoy_root}/lib/libavcodec.so" "")
    file(WRITE "${_decoy_root}/lib/pkgconfig/libavcodec.pc"
        "prefix=${_decoy_root}\n"
        "libdir=\${prefix}/lib\n"
        "includedir=\${prefix}/include\n"
        "Name: libavcodec\n"
        "Description: FindFFmpeg decoy fixture\n"
        "Version: 60.1.0\n"
        "Libs: -L\${libdir} -lavcodec\n"
        "Cflags: -I\${includedir}\n"
    )
    set(_pkg_config_libdir "${_decoy_root}/lib/pkgconfig")
endif()

if(_write_pkg_config)
    file(WRITE "${_package_root}/lib/pkgconfig/libavcodec.pc"
        "prefix=${_package_root}\n"
        "libdir=\${prefix}/lib\n"
        "includedir=\${prefix}/include\n"
        "Name: libavcodec\n"
        "Description: FindFFmpeg test fixture\n"
        "Version: ${_pkg_config_version}\n"
        "Libs: -L\${libdir} -lavcodec\n"
        "Libs.private: -lavutil -lprivate_dependency\n"
        "Cflags: -I\${includedir}\n"
    )
endif()

set(_command
    "${CMAKE_EXECUTABLE}"
    -E env
    "PKG_CONFIG_PATH="
    "PKG_CONFIG_LIBDIR=${_pkg_config_libdir}"
    "${CMAKE_EXECUTABLE}"
    -S "${FIXTURE_SOURCE_DIR}"
    -B "${_build_dir}"
    "-DCASE=${CASE}"
    "-DFIND_FFMPEG_MODULE_DIR=${FIND_FFMPEG_MODULE_DIR}"
    "-DFFMPEG_FIXTURE_ROOT=${_package_root}"
    "-DFFMPEG_ROOT=${_package_root}"
    "-DCMAKE_PREFIX_PATH=${_package_root}"
    ${_system_arguments}
)

execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

if(CASE STREQUAL "pkg_config_required")
    if(_result EQUAL 0 OR
       NOT "${_stdout}${_stderr}" MATCHES "Could NOT find FFmpeg" OR
       NOT "${_stdout}${_stderr}" MATCHES "AVCODEC")
        message(FATAL_ERROR
            "Missing FFmpeg pkg-config metadata was not rejected:\n"
            "${_stdout}${_stderr}"
        )
    endif()
elseif(CASE STREQUAL "old_avcodec")
    if(_result EQUAL 0 OR
       NOT "${_stdout}${_stderr}" MATCHES "Could NOT find FFmpeg" OR
       NOT "${_stdout}${_stderr}" MATCHES "AVCODEC")
        message(FATAL_ERROR
            "Old FFmpeg AVCODEC ABI was not rejected:\n"
            "${_stdout}${_stderr}"
        )
    endif()
elseif(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "FindFFmpeg ${CASE} fixture failed:\n${_stdout}${_stderr}"
    )
endif()
