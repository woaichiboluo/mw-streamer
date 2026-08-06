# FindFFmpeg.cmake
#
# Locate FFmpeg through its pkg-config metadata and verify the requested
# static or shared library artifacts.
#
# Inputs:
#
#   FFmpeg_ROOT
#   FFMPEG_LINKAGE  STATIC or SHARED
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

set(FFmpeg_ROOT "" CACHE PATH "FFmpeg installation root")
set(FFMPEG_LINKAGE "" CACHE STRING "FFmpeg linkage: STATIC or SHARED")
set_property(CACHE FFMPEG_LINKAGE PROPERTY STRINGS STATIC SHARED)

string(TOUPPER "${FFMPEG_LINKAGE}" _FFMPEG_LINKAGE)
if(NOT _FFMPEG_LINKAGE STREQUAL "STATIC" AND
   NOT _FFMPEG_LINKAGE STREQUAL "SHARED")
  message(FATAL_ERROR
    "FFMPEG_LINKAGE must be explicitly set to STATIC or SHARED")
endif()

find_package(PkgConfig REQUIRED)

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

function(_ffmpeg_component_metadata component output_library output_header)
  if(component STREQUAL "AVCODEC")
    set(_library avcodec)
    set(_header libavcodec/avcodec.h)
  elseif(component STREQUAL "AVDEVICE")
    set(_library avdevice)
    set(_header libavdevice/avdevice.h)
  elseif(component STREQUAL "AVFILTER")
    set(_library avfilter)
    set(_header libavfilter/avfilter.h)
  elseif(component STREQUAL "AVFORMAT")
    set(_library avformat)
    set(_header libavformat/avformat.h)
  elseif(component STREQUAL "AVUTIL")
    set(_library avutil)
    set(_header libavutil/avutil.h)
  elseif(component STREQUAL "SWRESAMPLE")
    set(_library swresample)
    set(_header libswresample/swresample.h)
  elseif(component STREQUAL "SWSCALE")
    set(_library swscale)
    set(_header libswscale/swscale.h)
  else()
    set(_library)
    set(_header)
  endif()

  set(${output_library} "${_library}" PARENT_SCOPE)
  set(${output_header} "${_header}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_find_pkg_config_file output package_name)
  if(NOT FFmpeg_ROOT)
    set(${output} "${package_name}" PARENT_SCOPE)
    return()
  endif()

  find_file(
    _pc_file
    NAMES "${package_name}.pc"
    HINTS "${FFmpeg_ROOT}"
    PATH_SUFFIXES lib/pkgconfig lib64/pkgconfig share/pkgconfig
    NO_DEFAULT_PATH
    NO_CACHE
  )
  set(${output} "${_pc_file}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_find_unix_library output library_name)
  if(_FFMPEG_LINKAGE STREQUAL "STATIC")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
  else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()

  unset(_ffmpeg_library)
  find_library(
    _ffmpeg_library
    NAMES "${library_name}"
    HINTS ${ARGN}
    NO_DEFAULT_PATH
    NO_CACHE
  )
  set(${output} "${_ffmpeg_library}" PARENT_SCOPE)
endfunction()

function(_ffmpeg_find_windows_file output)
  cmake_parse_arguments(ARG "" "" "NAMES;HINTS" ${ARGN})
  unset(_ffmpeg_file)
  find_file(
    _ffmpeg_file
    NAMES ${ARG_NAMES}
    HINTS ${ARG_HINTS}
    NO_DEFAULT_PATH
    NO_CACHE
  )
  set(${output} "${_ffmpeg_file}" PARENT_SCOPE)
endfunction()

set(_FFMPEG_FOUND_COMPONENTS)
set(FFMPEG_INCLUDE_DIRS)
set(FFMPEG_LIBRARIES)
set(FFMPEG_LIBRARY_DIRS)
set(FFMPEG_DEFINITIONS)

foreach(_requested_component IN LISTS FFmpeg_FIND_COMPONENTS)
  string(TOUPPER "${_requested_component}" _component)
  list(FIND _FFMPEG_SUPPORTED_COMPONENTS "${_component}" _component_index)
  _ffmpeg_component_metadata("${_component}" _library_name _header_name)

  if(_component_index EQUAL -1)
    set(FFmpeg_${_requested_component}_FOUND FALSE)
    set(FFmpeg_${_component}_FOUND FALSE)
    continue()
  endif()

  set(_pkg_config_name "lib${_library_name}")
  _ffmpeg_find_pkg_config_file(_pkg_config_spec "${_pkg_config_name}")
  if(_pkg_config_spec)
    set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    if(FFmpeg_ROOT)
      get_filename_component(_pkg_config_dir "${_pkg_config_spec}" DIRECTORY)
      set(_pkg_config_path "${_pkg_config_dir}")
      if(_saved_pkg_config_path)
        list(APPEND _pkg_config_path "${_saved_pkg_config_path}")
      endif()
      cmake_path(CONVERT "${_pkg_config_path}" TO_NATIVE_PATH_LIST
        _native_pkg_config_path NORMALIZE)
      set(ENV{PKG_CONFIG_PATH} "${_native_pkg_config_path}")
    endif()
    pkg_check_modules(PC_${_component} QUIET "${_pkg_config_spec}")
    set(ENV{PKG_CONFIG_PATH} "${_saved_pkg_config_path}")
  else()
    set(PC_${_component}_FOUND FALSE)
  endif()

  set(_include_dir)
  set(_library)
  set(_library_release)
  set(_library_debug)
  set(_dll)
  set(_dll_release)
  set(_dll_debug)

  if(PC_${_component}_FOUND)
    find_path(
      _include_dir
      NAMES "${_header_name}"
      HINTS
        ${PC_${_component}_INCLUDEDIR}
        ${PC_${_component}_INCLUDE_DIRS}
      NO_DEFAULT_PATH
      NO_CACHE
    )

    if(WIN32)
      set(_release_library_hints ${PC_${_component}_LIBRARY_DIRS})
      set(_debug_library_hints)
      set(_release_runtime_hints)
      set(_debug_runtime_hints)
      foreach(_library_dir IN LISTS PC_${_component}_LIBRARY_DIRS)
        get_filename_component(_prefix "${_library_dir}" DIRECTORY)
        list(APPEND _release_runtime_hints "${_prefix}/bin")
      endforeach()
      if(FFmpeg_ROOT)
        list(PREPEND _release_library_hints "${FFmpeg_ROOT}/lib")
        list(APPEND _debug_library_hints
          "${FFmpeg_ROOT}/debug/lib"
          "${FFmpeg_ROOT}/lib"
        )
        list(PREPEND _release_runtime_hints "${FFmpeg_ROOT}/bin")
        list(APPEND _debug_runtime_hints
          "${FFmpeg_ROOT}/debug/bin"
          "${FFmpeg_ROOT}/bin"
        )
      else()
        set(_debug_library_hints ${_release_library_hints})
        set(_debug_runtime_hints ${_release_runtime_hints})
      endif()

      if(_FFMPEG_LINKAGE STREQUAL "SHARED")
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
        _ffmpeg_find_windows_file(_library_release
          NAMES ${_import_library_names}
          HINTS ${_release_library_hints})
        _ffmpeg_find_windows_file(_library_debug
          NAMES ${_import_library_names}
          HINTS ${_debug_library_hints})
        _ffmpeg_find_windows_file(_dll_release
          NAMES ${_dll_names}
          HINTS ${_release_runtime_hints})
        _ffmpeg_find_windows_file(_dll_debug
          NAMES ${_dll_names}
          HINTS ${_debug_runtime_hints})

        if(NOT _library_release OR NOT _dll_release)
          set(_library_release)
          set(_dll_release)
        endif()
        if(NOT _library_debug OR NOT _dll_debug)
          set(_library_debug)
          set(_dll_debug)
        endif()
      else()
        set(_static_library_names
          "${_library_name}_static.lib"
          "lib${_library_name}.a"
          "${_library_name}.lib"
        )
        _ffmpeg_find_windows_file(_library_release
          NAMES ${_static_library_names}
          HINTS ${_release_library_hints})
        _ffmpeg_find_windows_file(_library_debug
          NAMES ${_static_library_names}
          HINTS ${_debug_library_hints})
      endif()

      if(_library_release)
        set(_library "${_library_release}")
        set(_dll "${_dll_release}")
      else()
        set(_library "${_library_debug}")
        set(_dll "${_dll_debug}")
      endif()
    else()
      _ffmpeg_find_unix_library(_library "${_library_name}"
        ${PC_${_component}_LIBRARY_DIRS})
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
  elseif(WIN32 AND _FFMPEG_LINKAGE STREQUAL "SHARED" AND NOT _dll)
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
      add_library(FFmpeg::${_lower_component}
        ${_FFMPEG_LINKAGE} IMPORTED GLOBAL)
      set_target_properties(
        FFmpeg::${_lower_component}
        PROPERTIES
          INTERFACE_COMPILE_OPTIONS "${PC_${_component}_CFLAGS_OTHER}"
          INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
      )

      if(WIN32)
        if(_FFMPEG_LINKAGE STREQUAL "SHARED")
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
          if(_FFMPEG_LINKAGE STREQUAL "SHARED")
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
          if(_FFMPEG_LINKAGE STREQUAL "SHARED")
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

      if(_FFMPEG_LINKAGE STREQUAL "STATIC")
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
            _ffmpeg_find_windows_file(_static_dependency
              NAMES
                "${_static_library}_static.lib"
                "lib${_static_library}.a"
                "${_static_library}.lib"
              HINTS ${PC_${_component}_STATIC_LIBRARY_DIRS})
          else()
            _ffmpeg_find_unix_library(_static_dependency
              "${_static_library}"
              ${PC_${_component}_STATIC_LIBRARY_DIRS})
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
    "Install FFmpeg pkg-config metadata and the requested ${_FFMPEG_LINKAGE} libraries"
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
