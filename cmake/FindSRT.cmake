# FindSRT.cmake
#
# Locate the shared Secure Reliable Transport (SRT) library through its
# pkg-config metadata.
#
# Result variables:
#
#   SRT_FOUND
#   SRT_VERSION
#   SRT_INCLUDE_DIRS
#   SRT_LIBRARY_DIRS
#   SRT_LIBRARIES
#   SRT_DEFINITIONS
#
# Cache variables:
#
#   SRT_INCLUDE_DIR
#   SRT_LIBRARY
#
# Imported target:
#
#   SRT::SRT
#
# Static SRT libraries are intentionally unsupported. Both pkg-config metadata
# and the shared library are required.

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SRT QUIET IMPORTED_TARGET GLOBAL srt)
endif()

find_path(
  SRT_INCLUDE_DIR
  NAMES srt/srt.h
  HINTS
    ${PC_SRT_INCLUDEDIR}
    ${PC_SRT_INCLUDE_DIRS}
  NO_DEFAULT_PATH
)

set(_SRT_LIBRARY_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES}")
if(WIN32 AND CMAKE_IMPORT_LIBRARY_SUFFIX)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_IMPORT_LIBRARY_SUFFIX}")
elseif(CMAKE_SHARED_LIBRARY_SUFFIX)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

find_library(
  SRT_LIBRARY
  NAMES srt
  HINTS
    ${PC_SRT_LIBDIR}
    ${PC_SRT_LIBRARY_DIRS}
  NO_DEFAULT_PATH
)

set(CMAKE_FIND_LIBRARY_SUFFIXES "${_SRT_LIBRARY_SUFFIXES}")

find_package_handle_standard_args(
  SRT
  REQUIRED_VARS SRT_LIBRARY SRT_INCLUDE_DIR PC_SRT_FOUND
  VERSION_VAR PC_SRT_VERSION
  REASON_FAILURE_MESSAGE "Install the shared libsrt library and its srt.pc file"
)

set(SRT_VERSION "${PC_SRT_VERSION}")
set(SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIR}")
set(SRT_LIBRARY_DIRS "${PC_SRT_LIBRARY_DIRS}")
set(SRT_LIBRARIES "${SRT_LIBRARY}")
set(SRT_DEFINITIONS "${PC_SRT_CFLAGS_OTHER}")

if(SRT_FOUND AND NOT TARGET SRT::SRT)
  add_library(SRT::SRT INTERFACE IMPORTED)
  set_target_properties(
    SRT::SRT
    PROPERTIES INTERFACE_LINK_LIBRARIES PkgConfig::PC_SRT
  )
  if(WIN32)
    set_property(
      TARGET SRT::SRT APPEND PROPERTY
      INTERFACE_COMPILE_DEFINITIONS SRT_DYNAMIC
    )
  endif()
endif()

mark_as_advanced(SRT_INCLUDE_DIR SRT_LIBRARY)
