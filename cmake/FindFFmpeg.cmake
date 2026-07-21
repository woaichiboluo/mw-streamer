# FindFFmpeg.cmake
#
# This file is derived from avcpp's FindFFmpeg.cmake:
# https://github.com/h4tr3d/avcpp
#
# Copyright (c) 2006, Matthias Kretz, <kretz@kde.org>
# Copyright (c) 2008, Alexander Neundorf, <neundorf@kde.org>
# Copyright (c) 2011, Michael Jansen, <kde@michael-jansen.biz>
# Copyright (c) 2017-2026, Alexander Drozdov, <adrozdoff@gmail.com>
#
# Redistribution and use is allowed according to the terms of the BSD
# license. The original license is available in avcpp's LICENSE-bsd.txt.
#
# Supported components:
#
#   AVCODEC AVFORMAT AVUTIL SWRESAMPLE SWSCALE
#
# Imported targets:
#
#   FFmpeg::avcodec
#   FFmpeg::avformat
#   FFmpeg::avutil
#   FFmpeg::swresample
#   FFmpeg::swscale
#   FFmpeg::FFmpeg
#
# pkg-config results are used only as search hints. Headers and libraries are
# always confirmed with find_path() and find_library().

include(FindPackageHandleStandardArgs)

set(_FFMPEG_SUPPORTED_COMPONENTS AVCODEC AVFORMAT AVUTIL SWRESAMPLE SWSCALE)

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS AVCODEC AVFORMAT AVUTIL)
  foreach(_default_component IN LISTS FFmpeg_FIND_COMPONENTS)
    set(FFmpeg_FIND_REQUIRED_${_default_component} TRUE)
  endforeach()
endif()

find_package(PkgConfig QUIET)

set(_FFMPEG_FOUND_COMPONENTS)
set(FFMPEG_INCLUDE_DIRS)
set(FFMPEG_LIBRARIES)
set(FFMPEG_LIBRARY_DIRS)
set(FFMPEG_DEFINITIONS)

foreach(_requested_component IN LISTS FFmpeg_FIND_COMPONENTS)
  string(TOUPPER "${_requested_component}" _component)
  list(FIND _FFMPEG_SUPPORTED_COMPONENTS "${_component}" _component_index)

  if(_component_index EQUAL -1)
    set(FFmpeg_${_requested_component}_FOUND FALSE)
    set(FFmpeg_${_component}_FOUND FALSE)
    continue()
  endif()

  if(_component STREQUAL "AVCODEC")
    set(_pkg_config_name libavcodec)
    set(_library_name avcodec)
    set(_header_name libavcodec/avcodec.h)
  elseif(_component STREQUAL "AVFORMAT")
    set(_pkg_config_name libavformat)
    set(_library_name avformat)
    set(_header_name libavformat/avformat.h)
  elseif(_component STREQUAL "AVUTIL")
    set(_pkg_config_name libavutil)
    set(_library_name avutil)
    set(_header_name libavutil/avutil.h)
  elseif(_component STREQUAL "SWRESAMPLE")
    set(_pkg_config_name libswresample)
    set(_library_name swresample)
    set(_header_name libswresample/swresample.h)
  elseif(_component STREQUAL "SWSCALE")
    set(_pkg_config_name libswscale)
    set(_library_name swscale)
    set(_header_name libswscale/swscale.h)
  endif()

  if(PkgConfig_FOUND)
    pkg_check_modules(PC_${_component} QUIET ${_pkg_config_name})
  endif()

  find_path(
    ${_component}_INCLUDE_DIR
    NAMES ${_header_name}
    HINTS
      ${PC_${_component}_INCLUDEDIR}
      ${PC_${_component}_INCLUDE_DIRS}
    PATH_SUFFIXES ffmpeg
  )
  find_library(
    ${_component}_LIBRARY
    NAMES ${_library_name}
    HINTS
      ${PC_${_component}_LIBDIR}
      ${PC_${_component}_LIBRARY_DIRS}
  )

  if(${_component}_INCLUDE_DIR AND ${_component}_LIBRARY)
    set(${_component}_FOUND TRUE)
    set(FFmpeg_${_requested_component}_FOUND TRUE)
    set(FFmpeg_${_component}_FOUND TRUE)
    set(${_component}_INCLUDE_DIRS "${${_component}_INCLUDE_DIR}")
    set(${_component}_LIBRARIES "${${_component}_LIBRARY}")
    set(${_component}_DEFINITIONS "${PC_${_component}_CFLAGS_OTHER}")
    set(${_component}_VERSION "${PC_${_component}_VERSION}")

    get_filename_component(
      ${_component}_LIBRARY_DIR "${${_component}_LIBRARY}" DIRECTORY
    )
    set(${_component}_LIBRARY_DIRS "${${_component}_LIBRARY_DIR}")

    list(APPEND _FFMPEG_FOUND_COMPONENTS "${_component}")
    list(APPEND FFMPEG_INCLUDE_DIRS "${${_component}_INCLUDE_DIR}")
    list(APPEND FFMPEG_LIBRARIES "${${_component}_LIBRARY}")
    list(APPEND FFMPEG_LIBRARY_DIRS "${${_component}_LIBRARY_DIR}")
    list(APPEND FFMPEG_DEFINITIONS ${PC_${_component}_CFLAGS_OTHER})
  else()
    set(${_component}_FOUND FALSE)
    set(FFmpeg_${_requested_component}_FOUND FALSE)
    set(FFmpeg_${_component}_FOUND FALSE)
  endif()

  mark_as_advanced(${_component}_INCLUDE_DIR ${_component}_LIBRARY)
endforeach()

foreach(_aggregate_variable IN ITEMS
        FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES FFMPEG_LIBRARY_DIRS
        FFMPEG_DEFINITIONS)
  if(${_aggregate_variable})
    list(REMOVE_DUPLICATES ${_aggregate_variable})
  endif()
endforeach()

find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS FFMPEG_LIBRARIES FFMPEG_INCLUDE_DIRS
  HANDLE_COMPONENTS
)
set(FFMPEG_FOUND ${FFmpeg_FOUND})

foreach(_component IN LISTS _FFMPEG_FOUND_COMPONENTS)
  string(TOLOWER "${_component}" _lower_component)
  if(NOT TARGET FFmpeg::${_lower_component})
    add_library(FFmpeg::${_lower_component} UNKNOWN IMPORTED)
    set_target_properties(
      FFmpeg::${_lower_component}
      PROPERTIES
        IMPORTED_LOCATION "${${_component}_LIBRARY}"
        INTERFACE_COMPILE_OPTIONS "${${_component}_DEFINITIONS}"
        INTERFACE_INCLUDE_DIRECTORIES "${${_component}_INCLUDE_DIR}"
    )
  endif()
endforeach()

if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
  set(_ffmpeg_component_targets)
  foreach(_component IN LISTS _FFMPEG_FOUND_COMPONENTS)
    string(TOLOWER "${_component}" _lower_component)
    list(APPEND _ffmpeg_component_targets FFmpeg::${_lower_component})
  endforeach()

  add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
  set_target_properties(
    FFmpeg::FFmpeg
    PROPERTIES INTERFACE_LINK_LIBRARIES "${_ffmpeg_component_targets}"
  )
endif()
