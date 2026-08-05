# FindSRT.cmake
#
# Locate the Secure Reliable Transport (SRT) library through its pkg-config
# metadata.
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
# Both pkg-config metadata and a linkable library are required. The platform's
# normal library suffix order decides whether a shared or static library is
# preferred.

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

find_library(
  SRT_LIBRARY
  NAMES srt
  HINTS
    ${PC_SRT_LIBDIR}
    ${PC_SRT_LIBRARY_DIRS}
  NO_DEFAULT_PATH
)

find_package_handle_standard_args(
  SRT
  REQUIRED_VARS SRT_LIBRARY SRT_INCLUDE_DIR PC_SRT_FOUND
  VERSION_VAR PC_SRT_VERSION
  REASON_FAILURE_MESSAGE "Install libsrt and its srt.pc file"
)

set(SRT_VERSION "${PC_SRT_VERSION}")
set(SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIR}")
set(SRT_LIBRARY_DIRS "${PC_SRT_LIBRARY_DIRS}")
set(SRT_LIBRARIES "${SRT_LIBRARY}")
set(SRT_DEFINITIONS "${PC_SRT_CFLAGS_OTHER}")

if(SRT_FOUND AND NOT TARGET SRT::SRT)
  add_library(SRT::SRT UNKNOWN IMPORTED)
  set_target_properties(
    SRT::SRT
    PROPERTIES
      IMPORTED_LOCATION "${SRT_LIBRARY}"
      INTERFACE_COMPILE_OPTIONS "${PC_SRT_CFLAGS_OTHER}"
      INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}"
  )
  if(UNIX AND "${SRT_LIBRARY}" MATCHES "\\.a$")
    set(_SRT_STATIC_LIBRARIES "${PC_SRT_STATIC_LIBRARIES}")
    list(REMOVE_ITEM _SRT_STATIC_LIBRARIES srt)
    set_target_properties(
      SRT::SRT
      PROPERTIES
        INTERFACE_LINK_DIRECTORIES "${PC_SRT_STATIC_LIBRARY_DIRS}"
        INTERFACE_LINK_LIBRARIES "${_SRT_STATIC_LIBRARIES}"
        INTERFACE_LINK_OPTIONS "${PC_SRT_STATIC_LDFLAGS_OTHER}"
    )
  endif()
endif()

mark_as_advanced(SRT_INCLUDE_DIR SRT_LIBRARY)
