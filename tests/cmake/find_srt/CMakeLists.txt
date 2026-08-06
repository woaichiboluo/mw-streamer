cmake_minimum_required(VERSION 3.22)

project(find_srt_fixture LANGUAGES NONE)

list(PREPEND CMAKE_MODULE_PATH "${FIND_SRT_MODULE_DIR}")
find_package(SRT REQUIRED)

get_target_property(_target_type SRT::SRT TYPE)
if(NOT _target_type STREQUAL EXPECTED_TARGET_TYPE)
    message(FATAL_ERROR
        "Expected SRT::SRT type ${EXPECTED_TARGET_TYPE}, got ${_target_type}"
    )
endif()

if(CASE STREQUAL "linux_shared")
    get_target_property(_location SRT::SRT IMPORTED_LOCATION)
    if(NOT _location STREQUAL "${SRT_ROOT}/lib/libsrt.so")
        message(FATAL_ERROR "Unexpected shared SRT location: ${_location}")
    endif()
elseif(CASE STREQUAL "linux_static" OR
       CASE STREQUAL "windows_static_pkg_config")
    get_target_property(_libraries SRT::SRT INTERFACE_LINK_LIBRARIES)
    if(NOT "ssl" IN_LIST _libraries OR NOT "crypto" IN_LIST _libraries)
        message(FATAL_ERROR
            "Static pkg-config dependencies were not propagated: ${_libraries}"
        )
    endif()
elseif(CASE STREQUAL "windows_shared")
    get_target_property(_implib SRT::SRT IMPORTED_IMPLIB_RELEASE)
    get_target_property(_location SRT::SRT IMPORTED_LOCATION_RELEASE)
    get_target_property(_definitions SRT::SRT INTERFACE_COMPILE_DEFINITIONS)
    if(NOT _implib STREQUAL "${SRT_ROOT}/lib/Release-x64/srt.lib")
        message(FATAL_ERROR "Unexpected SRT import library: ${_implib}")
    endif()
    if(NOT _location STREQUAL "${SRT_ROOT}/bin/Release-x64/srt.dll")
        message(FATAL_ERROR "Unexpected SRT DLL: ${_location}")
    endif()
    if(NOT "SRT_DYNAMIC" IN_LIST _definitions)
        message(FATAL_ERROR "SRT_DYNAMIC was not propagated")
    endif()
elseif(CASE STREQUAL "windows_static")
    get_target_property(_libraries SRT::SRT INTERFACE_LINK_LIBRARIES)
    get_target_property(_options SRT::SRT INTERFACE_LINK_OPTIONS)
    string(JOIN ";" _libraries_text ${_libraries})
    foreach(_dependency IN ITEMS libssl.lib libcrypto.lib crypt32.lib ws2_32.lib)
        if(NOT _libraries_text MATCHES "${_dependency}")
            message(FATAL_ERROR
                "Windows static dependency was not propagated: ${_dependency}"
            )
        endif()
    endforeach()
    if(NOT "/ignore:4099" IN_LIST _options)
        message(FATAL_ERROR "libsrt.props link options were not propagated")
    endif()
endif()
