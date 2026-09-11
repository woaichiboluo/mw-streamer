# FindFFmpeg.cmake
#
# Locate FFmpeg through its pkg-config metadata and verify the requested
# library artifacts.
#
# Inputs:
#
#   FFMPEG_ROOT  Optional FFmpeg installation prefix
#
# Supported components:
#
#   AVCODEC AVDEVICE AVFILTER AVFORMAT AVUTIL SWRESAMPLE SWSCALE
#
# Imported targets:
#
#   FFmpeg::avcodec
#   FFmpeg::avdevice
#   FFmpeg::avfilter
#   FFmpeg::avformat
#   FFmpeg::avutil
#   FFmpeg::swresample
#   FFmpeg::swscale
#   FFmpeg::FFmpeg

include(FindPackageHandleStandardArgs)

set(FFMPEG_ROOT "" CACHE PATH "FFmpeg installation prefix")

find_package(PkgConfig REQUIRED)

set(_FFMPEG_ROOT_INCLUDE_HINTS)
set(_FFMPEG_ROOT_LIBRARY_HINTS)
if(FFMPEG_ROOT)
  list(APPEND _FFMPEG_ROOT_INCLUDE_HINTS "${FFMPEG_ROOT}/include")
  list(APPEND _FFMPEG_ROOT_LIBRARY_HINTS
    "${FFMPEG_ROOT}/lib"
    "${FFMPEG_ROOT}/lib64"
  )
endif()

set(
  _FFMPEG_SUPPORTED_COMPONENTS
  AVCODEC
  AVDEVICE
  AVFILTER
  AVFORMAT
  AVUTIL
  SWRESAMPLE
  SWSCALE
)

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS AVCODEC AVFORMAT AVUTIL)
  foreach(_default_component IN LISTS FFmpeg_FIND_COMPONENTS)
    set(FFmpeg_FIND_REQUIRED_${_default_component} TRUE)
  endforeach()
endif()

function(_ffmpeg_component_metadata component output_library output_header
         output_minimum_version)
  if(component STREQUAL "AVCODEC")
    set(_library avcodec)
    set(_header libavcodec/avcodec.h)
    set(_minimum_version 59)
  elseif(component STREQUAL "AVDEVICE")
    set(_library avdevice)
    set(_header libavdevice/avdevice.h)
    set(_minimum_version 59)
  elseif(component STREQUAL "AVFILTER")
    set(_library avfilter)
    set(_header libavfilter/avfilter.h)
    set(_minimum_version 8)
  elseif(component STREQUAL "AVFORMAT")
    set(_library avformat)
    set(_header libavformat/avformat.h)
    set(_minimum_version 59)
  elseif(component STREQUAL "AVUTIL")
    set(_library avutil)
    set(_header libavutil/avutil.h)
    set(_minimum_version 57)
  elseif(component STREQUAL "SWRESAMPLE")
    set(_library swresample)
    set(_header libswresample/swresample.h)
    set(_minimum_version 4)
  elseif(component STREQUAL "SWSCALE")
    set(_library swscale)
    set(_header libswscale/swscale.h)
    set(_minimum_version 6)
  else()
    set(_library)
    set(_header)
    set(_minimum_version)
  endif()

  set(${output_library} "${_library}" PARENT_SCOPE)
  set(${output_header} "${_header}" PARENT_SCOPE)
  set(${output_minimum_version} "${_minimum_version}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_pkg_config_spec output package_name)
  if(FFMPEG_ROOT)
    unset(_ffmpeg_pc_file)
    find_file(
      _ffmpeg_pc_file
      NAMES "${package_name}.pc"
      HINTS "${FFMPEG_ROOT}"
      PATH_SUFFIXES lib/pkgconfig lib64/pkgconfig share/pkgconfig
      NO_DEFAULT_PATH
      NO_CACHE
    )
  endif()

  if(_ffmpeg_pc_file)
    set(${output} "${_ffmpeg_pc_file}" PARENT_SCOPE)
  else()
    set(${output} "${package_name}" PARENT_SCOPE)
  endif()
endfunction()

function(_ffmpeg_find_unix_library output library_name)
  unset(_ffmpeg_library)
  find_library(
    _ffmpeg_library
    NAMES "${library_name}"
    HINTS ${ARGN}
    NO_CACHE
  )
  set(${output} "${_ffmpeg_library}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_find_file output)
  cmake_parse_arguments(ARG "" "" "NAMES;HINTS" ${ARGN})
  unset(_ffmpeg_file)
  find_file(
    _ffmpeg_file
    NAMES ${ARG_NAMES}
    HINTS ${ARG_HINTS}
    NO_CACHE
  )
  set(${output} "${_ffmpeg_file}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_find_windows_runtime output)
  cmake_parse_arguments(ARG "" "" "NAMES;HINTS" ${ARGN})
  unset(_ffmpeg_runtime)
  find_file(
    _ffmpeg_runtime
    NAMES ${ARG_NAMES}
    HINTS ${ARG_HINTS}
    NO_DEFAULT_PATH
    NO_CACHE
  )
  set(${output} "${_ffmpeg_runtime}" PARENT_SCOPE)
endfunction()

set(_FFMPEG_FOUND_COMPONENTS)
set(FFMPEG_INCLUDE_DIRS)
set(FFMPEG_LIBRARIES)
set(FFMPEG_LIBRARY_DIRS)
set(FFMPEG_DEFINITIONS)

foreach(_requested_component IN LISTS FFmpeg_FIND_COMPONENTS)
  string(TOUPPER "${_requested_component}" _component)
  list(FIND _FFMPEG_SUPPORTED_COMPONENTS "${_component}" _component_index)
  _ffmpeg_component_metadata("${_component}" _library_name _header_name
    _minimum_version)

  if(_component_index EQUAL -1)
    set(FFmpeg_${_requested_component}_FOUND FALSE)
    set(FFmpeg_${_component}_FOUND FALSE)
    continue()
  endif()

  set(_pkg_config_name "lib${_library_name}")
  _ffmpeg_pkg_config_spec(_pkg_config_spec "${_pkg_config_name}")
  pkg_check_modules(PC_${_component} QUIET "${_pkg_config_spec}")

  set(_include_dir)
  set(_library)
  set(_library_release)
  set(_library_debug)
  set(_dll)
  set(_dll_release)
  set(_dll_debug)
  set(_component_static FALSE)

  if(PC_${_component}_FOUND)
    find_path(
      _include_dir
      NAMES "${_header_name}"
      HINTS
        ${_FFMPEG_ROOT_INCLUDE_HINTS}
        ${PC_${_component}_INCLUDEDIR}
        ${PC_${_component}_INCLUDE_DIRS}
      NO_CACHE
    )

    if(WIN32)
      set(_release_library_hints
        ${_FFMPEG_ROOT_LIBRARY_HINTS}
        ${PC_${_component}_LIBRARY_DIRS}
      )
      set(_debug_library_hints)
      set(_release_runtime_hints)
      set(_debug_runtime_hints)
      foreach(_library_dir IN LISTS PC_${_component}_LIBRARY_DIRS)
        get_filename_component(_prefix "${_library_dir}" DIRECTORY)
        list(APPEND _release_runtime_hints "${_prefix}/bin")
      endforeach()
      if(FFMPEG_ROOT)
        list(PREPEND _release_runtime_hints "${FFMPEG_ROOT}/bin")
      endif()
      set(_debug_library_hints ${_release_library_hints})
      set(_debug_runtime_hints ${_release_runtime_hints})

      string(REGEX MATCH "^[0-9]+" _version_major
        "${PC_${_component}_VERSION}")
      set(_import_library_names
        "${_library_name}.lib"
        "lib${_library_name}.dll.a"
        "${_library_name}.dll.a"
      )
      set(_dll_names
        "${_library_name}-${_version_major}.dll"
        "lib${_library_name}-${_version_major}.dll"
        "${_library_name}.dll"
        "lib${_library_name}.dll"
      )
      _ffmpeg_find_file(_library_release
        NAMES ${_import_library_names}
        HINTS ${_release_library_hints})
      _ffmpeg_find_windows_runtime(_dll_release
        NAMES ${_dll_names}
        HINTS ${_release_runtime_hints})

      if(_library_release AND _dll_release)
        set(_library "${_library_release}")
        set(_dll "${_dll_release}")
        set(_component_static FALSE)
      else()
        set(_library_release)
        set(_dll_release)
        set(_static_library_names
          "${_library_name}_static.lib"
          "lib${_library_name}.a"
          "${_library_name}.lib"
        )
        _ffmpeg_find_file(_library_release
          NAMES ${_static_library_names}
          HINTS ${_release_library_hints})
        _ffmpeg_find_file(_library_debug
          NAMES ${_static_library_names}
          HINTS ${_debug_library_hints})
        if(_library_release)
          set(_library "${_library_release}")
        else()
          set(_library "${_library_debug}")
        endif()
        set(_component_static TRUE)
      endif()
    else()
      _ffmpeg_find_unix_library(_library "${_library_name}"
        ${_FFMPEG_ROOT_LIBRARY_HINTS}
        ${PC_${_component}_LIBRARY_DIRS})
      if(_library MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
        set(_component_static TRUE)
      else()
        set(_component_static FALSE)
      endif()
    endif()
  endif()

  set(${_component}_INCLUDE_DIR "${_include_dir}" CACHE PATH
    "FFmpeg ${_library_name} include directory" FORCE)
  set(${_component}_LIBRARY "${_library}" CACHE FILEPATH
    "FFmpeg ${_library_name} library" FORCE)
  if(WIN32)
    set(${_component}_DLL "${_dll}" CACHE FILEPATH
      "FFmpeg ${_library_name} runtime library" FORCE)
  endif()

  set(_component_found TRUE)
  if(NOT PC_${_component}_FOUND OR NOT _include_dir OR NOT _library)
    set(_component_found FALSE)
  elseif(PC_${_component}_VERSION VERSION_LESS _minimum_version)
    set(_component_found FALSE)
  endif()

  set(${_component}_FOUND ${_component_found})
  set(FFmpeg_${_requested_component}_FOUND ${_component_found})
  set(FFmpeg_${_component}_FOUND ${_component_found})
  if(_component_found)
    set(${_component}_INCLUDE_DIRS "${_include_dir}")
    set(${_component}_LIBRARIES "${_library}")
    set(${_component}_DEFINITIONS "${PC_${_component}_CFLAGS_OTHER}")
    set(${_component}_VERSION "${PC_${_component}_VERSION}")

    get_filename_component(_library_dir "${_library}" DIRECTORY)
    set(${_component}_LIBRARY_DIR "${_library_dir}")
    set(${_component}_LIBRARY_DIRS "${_library_dir}")

    list(APPEND _FFMPEG_FOUND_COMPONENTS "${_component}")
    list(APPEND FFMPEG_INCLUDE_DIRS "${_include_dir}")
    list(APPEND FFMPEG_LIBRARIES "${_library}")
    list(APPEND FFMPEG_LIBRARY_DIRS "${_library_dir}")
    list(APPEND FFMPEG_DEFINITIONS ${PC_${_component}_CFLAGS_OTHER})

    string(TOLOWER "${_component}" _lower_component)
    if(NOT TARGET FFmpeg::${_lower_component})
      add_library(FFmpeg::${_lower_component} UNKNOWN IMPORTED GLOBAL)
      set_target_properties(
        FFmpeg::${_lower_component}
        PROPERTIES
          INTERFACE_COMPILE_OPTIONS "${PC_${_component}_CFLAGS_OTHER}"
          INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
      )

      if(WIN32)
        if(NOT _component_static)
          set_target_properties(
            FFmpeg::${_lower_component}
            PROPERTIES
              IMPORTED_IMPLIB "${_library}"
              IMPORTED_LOCATION "${_dll}"
          )
        else()
          set_property(TARGET FFmpeg::${_lower_component}
            PROPERTY IMPORTED_LOCATION "${_library}")
        endif()

        set(_imported_configurations)
        if(_library_release)
          list(APPEND _imported_configurations RELEASE)
          if(NOT _component_static)
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_IMPLIB_RELEASE "${_library_release}")
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_LOCATION_RELEASE "${_dll_release}")
          else()
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_LOCATION_RELEASE "${_library_release}")
          endif()
        endif()
        if(_library_debug)
          list(APPEND _imported_configurations DEBUG)
          if(NOT _component_static)
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_IMPLIB_DEBUG "${_library_debug}")
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_LOCATION_DEBUG "${_dll_debug}")
          else()
            set_property(TARGET FFmpeg::${_lower_component}
              PROPERTY IMPORTED_LOCATION_DEBUG "${_library_debug}")
          endif()
        endif()
        set_target_properties(
          FFmpeg::${_lower_component}
          PROPERTIES
            IMPORTED_CONFIGURATIONS "${_imported_configurations}"
            MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
        )
      else()
        set_property(TARGET FFmpeg::${_lower_component}
          PROPERTY IMPORTED_LOCATION "${_library}")
      endif()

      if(_component_static)
        set(_static_libraries ${PC_${_component}_STATIC_LIBRARIES})
        list(REMOVE_ITEM _static_libraries
          "${_library_name}" "lib${_library_name}")
        set(_ffmpeg_library_names
          avcodec avdevice avfilter avformat avutil swresample swscale)
        set(_resolved_static_libraries)
        foreach(_static_library IN LISTS _static_libraries)
          list(FIND _ffmpeg_library_names "${_static_library}"
            _ffmpeg_library_index)
          if(_ffmpeg_library_index EQUAL -1)
            list(APPEND _resolved_static_libraries "${_static_library}")
            continue()
          endif()

          if(WIN32)
            _ffmpeg_find_file(_static_dependency
              NAMES
                "${_static_library}_static.lib"
                "lib${_static_library}.a"
                "${_static_library}.lib"
              HINTS ${PC_${_component}_STATIC_LIBRARY_DIRS})
          else()
            _ffmpeg_find_file(_static_dependency
              NAMES "lib${_static_library}${CMAKE_STATIC_LIBRARY_SUFFIX}"
              HINTS ${PC_${_component}_STATIC_LIBRARY_DIRS})
          endif()
          if(NOT _static_dependency)
            message(FATAL_ERROR
              "Static FFmpeg dependency was not found: ${_static_library}")
          endif()
          list(APPEND _resolved_static_libraries "${_static_dependency}")
        endforeach()
        set_target_properties(
          FFmpeg::${_lower_component}
          PROPERTIES
            INTERFACE_COMPILE_OPTIONS
              "${PC_${_component}_STATIC_CFLAGS_OTHER}"
            INTERFACE_LINK_DIRECTORIES
              "${PC_${_component}_STATIC_LIBRARY_DIRS}"
            INTERFACE_LINK_LIBRARIES "${_resolved_static_libraries}"
            INTERFACE_LINK_OPTIONS
              "${PC_${_component}_STATIC_LDFLAGS_OTHER}"
        )
      endif()
    endif()
  endif()

  mark_as_advanced(
    ${_component}_INCLUDE_DIR
    ${_component}_LIBRARY
    ${_component}_DLL
  )
endforeach()

unset(_library)

foreach(_aggregate_variable IN ITEMS
        FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES FFMPEG_LIBRARY_DIRS
        FFMPEG_DEFINITIONS)
  if(${_aggregate_variable})
    list(REMOVE_DUPLICATES ${_aggregate_variable})
  endif()
endforeach()

find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS PkgConfig_FOUND
  HANDLE_COMPONENTS
  REASON_FAILURE_MESSAGE
    "Install FFmpeg pkg-config metadata and development libraries"
)
set(FFMPEG_FOUND ${FFmpeg_FOUND})

if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
  set(_ffmpeg_component_targets)
  foreach(_component IN LISTS _FFMPEG_FOUND_COMPONENTS)
    string(TOLOWER "${_component}" _lower_component)
    list(APPEND _ffmpeg_component_targets FFmpeg::${_lower_component})
  endforeach()

  add_library(FFmpeg::FFmpeg INTERFACE IMPORTED GLOBAL)
  set_target_properties(
    FFmpeg::FFmpeg
    PROPERTIES INTERFACE_LINK_LIBRARIES "${_ffmpeg_component_targets}"
  )
endif()
