if(NOT DEFINED CASE OR NOT DEFINED CMAKE_EXECUTABLE OR
   NOT DEFINED FIND_SRT_MODULE_DIR OR NOT DEFINED FIXTURE_SOURCE_DIR OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "FindSRT test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
set(_package_root "${TEST_ROOT}/package")
set(_build_dir "${TEST_ROOT}/build")
file(MAKE_DIRECTORY "${_package_root}/include/srt")
file(WRITE "${_package_root}/include/srt/srt.h" "")
file(WRITE "${_package_root}/include/srt/version.h"
    "#define SRT_VERSION_STRING \"1.5.6\"\n"
)

set(_linkage)
set(_system_arguments)
set(_expected_target_type)
if(CASE STREQUAL "linkage_required")
    file(MAKE_DIRECTORY "${_package_root}/lib")
    file(WRITE "${_package_root}/lib/libsrt.so" "")
elseif(CASE STREQUAL "linux_shared")
    set(_linkage SHARED)
    set(_expected_target_type SHARED_LIBRARY)
    file(MAKE_DIRECTORY "${_package_root}/lib")
    file(WRITE "${_package_root}/lib/libsrt.so" "")
elseif(CASE STREQUAL "linux_static")
    set(_linkage STATIC)
    set(_expected_target_type STATIC_LIBRARY)
    file(MAKE_DIRECTORY "${_package_root}/lib/pkgconfig")
    file(WRITE "${_package_root}/lib/libsrt.a" "")
    file(WRITE "${_package_root}/lib/pkgconfig/srt.pc"
        "prefix=${_package_root}\n"
        "libdir=\${prefix}/lib\n"
        "includedir=\${prefix}/include\n"
        "Name: srt\n"
        "Description: FindSRT test fixture\n"
        "Version: 1.5.6\n"
        "Libs: -L\${libdir} -lsrt\n"
        "Libs.private: -lssl -lcrypto -pthread\n"
        "Cflags: -I\${includedir}\n"
    )
elseif(CASE STREQUAL "windows_shared")
    set(_linkage SHARED)
    set(_expected_target_type SHARED_LIBRARY)
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(MAKE_DIRECTORY
        "${_package_root}/lib/Release-x64"
        "${_package_root}/bin/Release-x64"
    )
    file(WRITE "${_package_root}/lib/Release-x64/srt.lib" "")
    file(WRITE "${_package_root}/bin/Release-x64/srt.dll" "")
elseif(CASE STREQUAL "windows_static")
    set(_linkage STATIC)
    set(_expected_target_type STATIC_LIBRARY)
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(MAKE_DIRECTORY "${_package_root}/lib/Release-x64")
    foreach(_library IN ITEMS srt.lib libssl.lib libcrypto.lib)
        file(WRITE "${_package_root}/lib/Release-x64/${_library}" "")
    endforeach()
    file(WRITE "${_package_root}/libsrt.props"
        "<Project><ItemDefinitionGroup><Link>"
        "<AdditionalDependencies>"
        "srt.lib;libssl.lib;libcrypto.lib;crypt32.lib;ws2_32.lib;"
        "%(AdditionalDependencies)"
        "</AdditionalDependencies>"
        "<AdditionalOptions>/ignore:4099 %(AdditionalOptions)</AdditionalOptions>"
        "</Link></ItemDefinitionGroup></Project>"
    )
elseif(CASE STREQUAL "windows_static_pkg_config")
    set(_linkage STATIC)
    set(_expected_target_type STATIC_LIBRARY)
    list(APPEND _system_arguments
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
    )
    file(MAKE_DIRECTORY "${_package_root}/lib/pkgconfig")
    file(WRITE "${_package_root}/lib/srt.lib" "")
    file(WRITE "${_package_root}/lib/pkgconfig/srt.pc"
        "prefix=${_package_root}\n"
        "libdir=\${prefix}/lib\n"
        "includedir=\${prefix}/include\n"
        "Name: srt\n"
        "Description: FindSRT Windows static fixture\n"
        "Version: 1.5.6\n"
        "Libs: -L\${libdir} -lsrt\n"
        "Libs.private: -lssl -lcrypto -lws2_32\n"
        "Cflags: -I\${includedir}\n"
    )
else()
    message(FATAL_ERROR "Unknown FindSRT test case: ${CASE}")
endif()

set(_command
    "${CMAKE_EXECUTABLE}"
    -E env
    "PKG_CONFIG_PATH=${_package_root}/lib/pkgconfig"
    "PKG_CONFIG_LIBDIR=${_package_root}/lib/pkgconfig"
    "${CMAKE_EXECUTABLE}"
    -S "${FIXTURE_SOURCE_DIR}"
    -B "${_build_dir}"
    "-DCASE=${CASE}"
    "-DEXPECTED_TARGET_TYPE=${_expected_target_type}"
    "-DFIND_SRT_MODULE_DIR=${FIND_SRT_MODULE_DIR}"
    "-DSRT_ROOT=${_package_root}"
    ${_system_arguments}
)
if(_linkage)
    list(APPEND _command "-DSRT_LINKAGE=${_linkage}")
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
           "SRT_LINKAGE must be explicitly set to STATIC or SHARED")
        message(FATAL_ERROR
            "Missing SRT_LINKAGE was not rejected as expected:\n"
            "${_stdout}${_stderr}"
        )
    endif()
elseif(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "FindSRT ${CASE} fixture failed:\n${_stdout}${_stderr}"
    )
endif()
