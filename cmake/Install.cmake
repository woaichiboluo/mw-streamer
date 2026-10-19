include(CMakePackageConfigHelpers)

set_target_properties(mw_log PROPERTIES EXPORT_NAME log)
set_target_properties(mw_streamer PROPERTIES EXPORT_NAME streamer)
if(BUILD_SHARED)
    set_target_properties(mw_log mw_streamer PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(mw_log mw_streamer PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
endif()

install(TARGETS mw_log mw_streamer EXPORT mw_streamerTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# These implementation headers are not part of the exported SDK API.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h"
    PATTERN "internal" EXCLUDE
    PATTERN "remux_output.h" EXCLUDE
    PATTERN "packet_converter.h" EXCLUDE
    PATTERN "packet_interleaver.h" EXCLUDE
    PATTERN "media_timestamp_converter.h" EXCLUDE
    PATTERN "syncer_timing.h" EXCLUDE
    PATTERN "latency.h" EXCLUDE
    PATTERN "runtime.h" EXCLUDE)

if(NOT BUILD_SHARED)
    # The bundled SDK has no relocatable export of its own. Preserve its target
    # graph (including static-library cycles), without exposing build paths.
    set(mw_bundled_targets zlmediakit ext-codec zlmediakit_srt_adapter
        ZLToolKit ZLToolKit_deps mov flv mpeg)
    get_target_property(mw_toolkit_includes ZLToolKit INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET ZLToolKit PROPERTY INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${mw_toolkit_includes}>")
    get_target_property(mw_toolkit_system_includes ZLToolKit
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    if(mw_toolkit_system_includes)
        set_property(TARGET ZLToolKit PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${mw_toolkit_system_includes}>")
    endif()
    foreach(mw_target IN ITEMS zlmediakit ZLToolKit_deps)
        get_target_property(mw_links ${mw_target} INTERFACE_LINK_LIBRARIES)
        list(REMOVE_ITEM mw_links ${OPENSSL_LIBRARIES})
        list(APPEND mw_links OpenSSL::SSL OpenSSL::Crypto)
        set_property(TARGET ${mw_target} PROPERTY INTERFACE_LINK_LIBRARIES "${mw_links}")
    endforeach()
    foreach(mw_target IN LISTS mw_bundled_targets)
        set_property(TARGET ${mw_target} PROPERTY EXPORT_NAME "detail_${mw_target}")
    endforeach()
    install(TARGETS ${mw_bundled_targets} EXPORT mw_streamerTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/mw_streamer")
endif()
install(FILES "${PROJECT_SOURCE_DIR}/third_party/ZLMediaKit/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mw_streamer/ZLMediaKit")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/nv-codec-headers/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mw_streamer/nv-codec-headers")

set(mw_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/mw_streamer")
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/mw_streamerConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/mw_streamerConfig.cmake"
    INSTALL_DESTINATION "${mw_package_dir}")
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/mw_streamerConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(EXPORT mw_streamerTargets NAMESPACE mw::
    DESTINATION "${mw_package_dir}")
install(FILES
    "${PROJECT_BINARY_DIR}/mw_streamerConfig.cmake"
    "${PROJECT_BINARY_DIR}/mw_streamerConfigVersion.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/FindFFmpeg.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/FindSRT.cmake"
    DESTINATION "${mw_package_dir}")

if(BUILD_FRAME_CONVTER)
    set_target_properties(mw_frame_convter PROPERTIES EXPORT_NAME frame_convter)
    if(BUILD_SHARED)
        set_target_properties(mw_frame_convter PROPERTIES
            VERSION "${PROJECT_VERSION}" SOVERSION "${PROJECT_VERSION_MAJOR}")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_target_properties(mw_frame_convter PROPERTIES INSTALL_RPATH "$ORIGIN")
        endif()
    endif()
    install(TARGETS mw_frame_convter EXPORT mw_frame_convterTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    set(mw_frame_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/mw_frame_convter")
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/mw_frame_convterConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/mw_frame_convterConfig.cmake"
        INSTALL_DESTINATION "${mw_frame_package_dir}")
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/mw_frame_convterConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
    install(EXPORT mw_frame_convterTargets NAMESPACE mw::
        DESTINATION "${mw_frame_package_dir}")
    install(FILES
        "${PROJECT_BINARY_DIR}/mw_frame_convterConfig.cmake"
        "${PROJECT_BINARY_DIR}/mw_frame_convterConfigVersion.cmake"
        "${PROJECT_SOURCE_DIR}/cmake/FindFFmpeg.cmake"
        DESTINATION "${mw_frame_package_dir}")
endif()
