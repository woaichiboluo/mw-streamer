# FindSRT.cmake
#
# Locate the Secure Reliable Transport (SRT) library. The caller must select
# static or shared linkage explicitly because a Windows .lib file can be either
# a static library or a DLL import library.
#
# Inputs:
#
#   SRT_ROOT
#   SRT_LINKAGE  STATIC or SHARED
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
#   SRT_DLL
#
# Imported target:
#
#   SRT::SRT
#
# pkg-config is optional. When available, its static metadata supplies SRT's
# private link dependencies. The official Windows installer has no srt.pc, so
# its libsrt.props file is used as the static dependency manifest instead.

include(FindPackageHandleStandardArgs)

set(SRT_ROOT "" CACHE PATH "libsrt installation root")
set(SRT_LINKAGE "" CACHE STRING "SRT linkage: STATIC or SHARED")
set_property(CACHE SRT_LINKAGE PROPERTY STRINGS STATIC SHARED)

string(TOUPPER "${SRT_LINKAGE}" _SRT_LINKAGE)
if(NOT _SRT_LINKAGE STREQUAL "STATIC" AND
   NOT _SRT_LINKAGE STREQUAL "SHARED")
  message(FATAL_ERROR
    "SRT_LINKAGE must be explicitly set to STATIC or SHARED")
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SRT QUIET srt)
endif()

set(_SRT_FIND_OPTIONS)
set(_SRT_INCLUDE_HINTS)
set(_SRT_LIBRARY_HINTS)
if(SRT_ROOT)
  list(APPEND _SRT_INCLUDE_HINTS "${SRT_ROOT}")
  list(APPEND _SRT_LIBRARY_HINTS "${SRT_ROOT}")
  list(APPEND _SRT_FIND_OPTIONS NO_DEFAULT_PATH)
else()
  list(APPEND _SRT_INCLUDE_HINTS
    ${PC_SRT_INCLUDEDIR}
    ${PC_SRT_INCLUDE_DIRS}
  )
  list(APPEND _SRT_LIBRARY_HINTS
    ${PC_SRT_LIBDIR}
    ${PC_SRT_LIBRARY_DIRS}
  )
endif()

find_path(
  SRT_INCLUDE_DIR
  NAMES srt/srt.h
  HINTS ${_SRT_INCLUDE_HINTS}
  PATH_SUFFIXES include
  ${_SRT_FIND_OPTIONS}
)

set(SRT_VERSION "${PC_SRT_VERSION}")
if(NOT SRT_VERSION AND SRT_INCLUDE_DIR)
  set(_SRT_VERSION_HEADER "${SRT_INCLUDE_DIR}/srt/version.h")
  if(EXISTS "${_SRT_VERSION_HEADER}")
    file(STRINGS "${_SRT_VERSION_HEADER}" _SRT_VERSION_LINE
      REGEX "^#define[ \t]+SRT_VERSION_STRING[ \t]+\"[^\"]+\"")
    string(REGEX REPLACE
      ".*SRT_VERSION_STRING[ \t]+\"([^\"]+)\".*" "\\1"
      SRT_VERSION "${_SRT_VERSION_LINE}")
  endif()
endif()

function(_srt_find_unix_library output linkage)
  if(linkage STREQUAL "STATIC")
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
  else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()

  unset(_srt_library)
  find_library(
    _srt_library
    NAMES srt
    HINTS ${_SRT_LIBRARY_HINTS}
    PATH_SUFFIXES lib lib64
    ${_SRT_FIND_OPTIONS}
    NO_CACHE
  )
  set(${output} "${_srt_library}" PARENT_SCOPE)
endfunction()

function(_srt_windows_platform output)
  if(CMAKE_VS_PLATFORM_NAME)
    set(_platform "${CMAKE_VS_PLATFORM_NAME}")
  elseif(CMAKE_GENERATOR_PLATFORM)
    set(_platform "${CMAKE_GENERATOR_PLATFORM}")
  else()
    set(_platform "${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  string(TOLOWER "${_platform}" _platform_lower)
  if(_platform_lower MATCHES "^(x64|amd64|x86_64)$")
    set(_platform "x64")
  elseif(_platform_lower MATCHES "^(win32|x86|i[3-6]86)$")
    set(_platform "Win32")
  elseif(_platform_lower MATCHES "^(arm64|aarch64)$")
    set(_platform "Arm64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_platform "x64")
  else()
    set(_platform "Win32")
  endif()
  set(${output} "${_platform}" PARENT_SCOPE)
endfunction()

function(_srt_find_windows_file output)
  cmake_parse_arguments(ARG "" "" "NAMES;HINTS" ${ARGN})
  unset(_srt_file)
  find_file(
    _srt_file
    NAMES ${ARG_NAMES}
    HINTS ${ARG_HINTS}
    ${_SRT_FIND_OPTIONS}
    NO_CACHE
  )
  set(${output} "${_srt_file}" PARENT_SCOPE)
endfunction()

function(_srt_parse_windows_props props_file output_dependencies
         output_options)
  file(READ "${props_file}" _contents)

  string(REGEX MATCH
    "<AdditionalDependencies>([^<]+)</AdditionalDependencies>"
    _dependencies_match "${_contents}")
  set(_dependencies "${CMAKE_MATCH_1}")
  string(REPLACE "%(AdditionalDependencies)" "" _dependencies
    "${_dependencies}")
  list(FILTER _dependencies EXCLUDE REGEX "^$")

  string(REGEX MATCH
    "<AdditionalOptions>([^<]+)</AdditionalOptions>"
    _options_match "${_contents}")
  set(_options "${CMAKE_MATCH_1}")
  string(REPLACE "%(AdditionalOptions)" "" _options "${_options}")
  separate_arguments(_options WINDOWS_COMMAND "${_options}")

  set(${output_dependencies} "${_dependencies}" PARENT_SCOPE)
  set(${output_options} "${_options}" PARENT_SCOPE)
endfunction()

function(_srt_path_is_in_directories output path)
  file(REAL_PATH "${path}" _path)
  set(_found FALSE)
  foreach(_directory IN LISTS ARGN)
    if(NOT _directory)
      continue()
    endif()
    file(REAL_PATH "${_directory}" _directory)
    string(FIND "${_path}" "${_directory}/" _position)
    if(_position EQUAL 0)
      set(_found TRUE)
      break()
    endif()
  endforeach()
  set(${output} ${_found} PARENT_SCOPE)
endfunction()

set(_SRT_LIBRARY_RELEASE)
set(_SRT_LIBRARY_DEBUG)
set(_SRT_DLL_RELEASE)
set(_SRT_DLL_DEBUG)
set(_SRT_PROPS_FILE)

if(WIN32)
  _srt_windows_platform(_SRT_WINDOWS_PLATFORM)

  set(_SRT_RELEASE_LIBRARY_HINTS ${PC_SRT_LIBRARY_DIRS})
  set(_SRT_DEBUG_LIBRARY_HINTS ${PC_SRT_LIBRARY_DIRS})
  set(_SRT_RELEASE_RUNTIME_HINTS)
  set(_SRT_DEBUG_RUNTIME_HINTS)
  if(SRT_ROOT)
    list(PREPEND _SRT_RELEASE_LIBRARY_HINTS
      "${SRT_ROOT}/lib/Release-${_SRT_WINDOWS_PLATFORM}"
      "${SRT_ROOT}/lib"
    )
    list(PREPEND _SRT_DEBUG_LIBRARY_HINTS
      "${SRT_ROOT}/lib/Debug-${_SRT_WINDOWS_PLATFORM}"
      "${SRT_ROOT}/debug/lib"
      "${SRT_ROOT}/lib"
    )
    list(APPEND _SRT_RELEASE_RUNTIME_HINTS
      "${SRT_ROOT}/bin/Release-${_SRT_WINDOWS_PLATFORM}"
      "${SRT_ROOT}/bin"
    )
    list(APPEND _SRT_DEBUG_RUNTIME_HINTS
      "${SRT_ROOT}/bin/Debug-${_SRT_WINDOWS_PLATFORM}"
      "${SRT_ROOT}/debug/bin"
      "${SRT_ROOT}/bin"
    )
  endif()

  if(SRT_ROOT AND EXISTS "${SRT_ROOT}/libsrt.props")
    set(_SRT_PROPS_FILE "${SRT_ROOT}/libsrt.props")
  endif()

  if(_SRT_LINKAGE STREQUAL "SHARED")
    _srt_find_windows_file(_SRT_LIBRARY_RELEASE
      NAMES srt.lib libsrt.dll.a
      HINTS ${_SRT_RELEASE_LIBRARY_HINTS})
    _srt_find_windows_file(_SRT_LIBRARY_DEBUG
      NAMES srt.lib libsrt.dll.a
      HINTS ${_SRT_DEBUG_LIBRARY_HINTS})
    _srt_find_windows_file(_SRT_DLL_RELEASE
      NAMES srt.dll libsrt.dll
      HINTS ${_SRT_RELEASE_RUNTIME_HINTS})
    _srt_find_windows_file(_SRT_DLL_DEBUG
      NAMES srt.dll libsrt.dll
      HINTS ${_SRT_DEBUG_RUNTIME_HINTS})

    # A Windows shared library is usable only when the DLL and its import
    # library are both present for the same configuration.
    if(NOT _SRT_LIBRARY_RELEASE OR NOT _SRT_DLL_RELEASE)
      set(_SRT_LIBRARY_RELEASE)
      set(_SRT_DLL_RELEASE)
    endif()
    if(NOT _SRT_LIBRARY_DEBUG OR NOT _SRT_DLL_DEBUG)
      set(_SRT_LIBRARY_DEBUG)
      set(_SRT_DLL_DEBUG)
    endif()
  else()
    set(_SRT_STATIC_NAMES srt_static.lib libsrt.a srt.lib)
    _srt_find_windows_file(_SRT_LIBRARY_RELEASE
      NAMES ${_SRT_STATIC_NAMES}
      HINTS ${_SRT_RELEASE_LIBRARY_HINTS})
    _srt_find_windows_file(_SRT_LIBRARY_DEBUG
      NAMES ${_SRT_STATIC_NAMES}
      HINTS ${_SRT_DEBUG_LIBRARY_HINTS})
  endif()

  if(_SRT_LIBRARY_RELEASE)
    set(_SRT_SELECTED_LIBRARY "${_SRT_LIBRARY_RELEASE}")
    set(_SRT_SELECTED_DLL "${_SRT_DLL_RELEASE}")
  else()
    set(_SRT_SELECTED_LIBRARY "${_SRT_LIBRARY_DEBUG}")
    set(_SRT_SELECTED_DLL "${_SRT_DLL_DEBUG}")
  endif()
  set(SRT_LIBRARY "${_SRT_SELECTED_LIBRARY}" CACHE FILEPATH
    "SRT library" FORCE)
  set(SRT_DLL "${_SRT_SELECTED_DLL}" CACHE FILEPATH
    "SRT runtime library" FORCE)
else()
  _srt_find_unix_library(SRT_LIBRARY "${_SRT_LINKAGE}")
  set(SRT_LIBRARY "${SRT_LIBRARY}" CACHE FILEPATH "SRT library" FORCE)
endif()

set(_SRT_DEPENDENCIES_FOUND TRUE)
set(_SRT_STATIC_DEPENDENCIES)
set(_SRT_STATIC_LIBRARY_DIRS)
set(_SRT_STATIC_LINK_OPTIONS)
set(_SRT_STATIC_COMPILE_OPTIONS)

if(_SRT_LINKAGE STREQUAL "STATIC" AND SRT_LIBRARY)
  _srt_path_is_in_directories(_SRT_PC_MATCHES_LIBRARY "${SRT_LIBRARY}"
    ${PC_SRT_LIBRARY_DIRS})

  if(PC_SRT_FOUND AND _SRT_PC_MATCHES_LIBRARY)
    set(_SRT_STATIC_DEPENDENCIES ${PC_SRT_STATIC_LIBRARIES})
    list(REMOVE_ITEM _SRT_STATIC_DEPENDENCIES srt libsrt)
    set(_SRT_STATIC_LIBRARY_DIRS ${PC_SRT_STATIC_LIBRARY_DIRS})
    set(_SRT_STATIC_LINK_OPTIONS ${PC_SRT_STATIC_LDFLAGS_OTHER})
    set(_SRT_STATIC_COMPILE_OPTIONS ${PC_SRT_STATIC_CFLAGS_OTHER})
  elseif(WIN32 AND _SRT_PROPS_FILE)
    _srt_parse_windows_props("${_SRT_PROPS_FILE}"
      _SRT_PROPS_DEPENDENCIES _SRT_STATIC_LINK_OPTIONS)
    if(NOT _SRT_PROPS_DEPENDENCIES)
      set(_SRT_DEPENDENCIES_FOUND FALSE)
    endif()
  else()
    set(_SRT_DEPENDENCIES_FOUND FALSE)
  endif()
endif()

set(_SRT_REQUIRED_VARS SRT_INCLUDE_DIR SRT_LIBRARY)
if(_SRT_LINKAGE STREQUAL "SHARED" AND WIN32)
  list(APPEND _SRT_REQUIRED_VARS SRT_DLL)
endif()
if(_SRT_LINKAGE STREQUAL "STATIC")
  list(APPEND _SRT_REQUIRED_VARS _SRT_DEPENDENCIES_FOUND)
endif()

if(_SRT_LINKAGE STREQUAL "STATIC" AND
   NOT _SRT_DEPENDENCIES_FOUND)
  set(_SRT_FAILURE_REASON
    "Static SRT requires matching srt.pc metadata or the official Windows libsrt.props package")
elseif(_SRT_LINKAGE STREQUAL "SHARED" AND WIN32)
  set(_SRT_FAILURE_REASON
    "Shared SRT on Windows requires both srt.dll and its srt.lib import library")
else()
  set(_SRT_FAILURE_REASON
    "Install the requested SRT ${_SRT_LINKAGE} development library")
endif()

find_package_handle_standard_args(
  SRT
  REQUIRED_VARS ${_SRT_REQUIRED_VARS}
  VERSION_VAR SRT_VERSION
  REASON_FAILURE_MESSAGE "${_SRT_FAILURE_REASON}"
)

set(SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIR}")
set(SRT_LIBRARIES "${SRT_LIBRARY}")
set(SRT_DEFINITIONS "${PC_SRT_CFLAGS_OTHER}")
if(_SRT_LINKAGE STREQUAL "SHARED" AND WIN32)
  list(APPEND SRT_DEFINITIONS SRT_DYNAMIC)
endif()
get_filename_component(SRT_LIBRARY_DIRS "${SRT_LIBRARY}" DIRECTORY)

if(SRT_FOUND AND NOT TARGET SRT::SRT)
  add_library(SRT::SRT ${_SRT_LINKAGE} IMPORTED GLOBAL)
  set_target_properties(
    SRT::SRT
    PROPERTIES
      INTERFACE_COMPILE_OPTIONS "${PC_SRT_CFLAGS_OTHER}"
      INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}"
  )

  if(_SRT_LINKAGE STREQUAL "SHARED" AND WIN32)
    set_target_properties(
      SRT::SRT
      PROPERTIES
        IMPORTED_IMPLIB "${SRT_LIBRARY}"
        IMPORTED_LOCATION "${SRT_DLL}"
    )
  else()
    set_property(TARGET SRT::SRT PROPERTY
      IMPORTED_LOCATION "${SRT_LIBRARY}")
  endif()

  if(WIN32)
    set(_SRT_IMPORTED_CONFIGURATIONS)
    if(_SRT_LIBRARY_RELEASE)
      list(APPEND _SRT_IMPORTED_CONFIGURATIONS RELEASE)
      if(_SRT_LINKAGE STREQUAL "SHARED")
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_IMPLIB_RELEASE "${_SRT_LIBRARY_RELEASE}")
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_LOCATION_RELEASE "${_SRT_DLL_RELEASE}")
      else()
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_LOCATION_RELEASE "${_SRT_LIBRARY_RELEASE}")
      endif()
    endif()
    if(_SRT_LIBRARY_DEBUG)
      list(APPEND _SRT_IMPORTED_CONFIGURATIONS DEBUG)
      if(_SRT_LINKAGE STREQUAL "SHARED")
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_IMPLIB_DEBUG "${_SRT_LIBRARY_DEBUG}")
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_LOCATION_DEBUG "${_SRT_DLL_DEBUG}")
      else()
        set_property(TARGET SRT::SRT PROPERTY
          IMPORTED_LOCATION_DEBUG "${_SRT_LIBRARY_DEBUG}")
      endif()
    endif()
    set_target_properties(
      SRT::SRT
      PROPERTIES
        IMPORTED_CONFIGURATIONS "${_SRT_IMPORTED_CONFIGURATIONS}"
        MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
    )
  endif()

  if(_SRT_LINKAGE STREQUAL "SHARED")
    if(WIN32)
      set_property(TARGET SRT::SRT APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS SRT_DYNAMIC)
      set_property(TARGET SRT::SRT APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Ws2_32)
    endif()
  elseif(PC_SRT_FOUND AND _SRT_PC_MATCHES_LIBRARY)
    set_target_properties(
      SRT::SRT
      PROPERTIES
        INTERFACE_COMPILE_OPTIONS "${_SRT_STATIC_COMPILE_OPTIONS}"
        INTERFACE_LINK_DIRECTORIES "${_SRT_STATIC_LIBRARY_DIRS}"
        INTERFACE_LINK_LIBRARIES "${_SRT_STATIC_DEPENDENCIES}"
        INTERFACE_LINK_OPTIONS "${_SRT_STATIC_LINK_OPTIONS}"
    )
  elseif(WIN32 AND _SRT_PROPS_FILE)
    set(_SRT_WINDOWS_SYSTEM_LIBRARIES crypt32.lib ws2_32.lib)
    set(_SRT_PROPS_LINK_LIBRARIES)
    get_filename_component(_SRT_RELEASE_LIBRARY_DIR
      "${_SRT_LIBRARY_RELEASE}" DIRECTORY)
    get_filename_component(_SRT_DEBUG_LIBRARY_DIR
      "${_SRT_LIBRARY_DEBUG}" DIRECTORY)

    foreach(_dependency IN LISTS _SRT_PROPS_DEPENDENCIES)
      string(STRIP "${_dependency}" _dependency)
      if(NOT _dependency OR _dependency STREQUAL "srt.lib")
        continue()
      endif()

      list(FIND _SRT_WINDOWS_SYSTEM_LIBRARIES "${_dependency}"
        _system_library_index)
      if(NOT _system_library_index EQUAL -1)
        list(APPEND _SRT_PROPS_LINK_LIBRARIES "${_dependency}")
        continue()
      endif()

      set(_release_dependency
        "${_SRT_RELEASE_LIBRARY_DIR}/${_dependency}")
      set(_debug_dependency "${_SRT_DEBUG_LIBRARY_DIR}/${_dependency}")
      if(_SRT_LIBRARY_RELEASE AND NOT EXISTS "${_release_dependency}")
        message(FATAL_ERROR
          "SRT static dependency does not exist: ${_release_dependency}")
      endif()
      if(_SRT_LIBRARY_DEBUG AND NOT EXISTS "${_debug_dependency}")
        message(FATAL_ERROR
          "SRT static dependency does not exist: ${_debug_dependency}")
      endif()
      if(_SRT_LIBRARY_RELEASE)
        list(APPEND _SRT_PROPS_LINK_LIBRARIES
          "$<$<NOT:$<CONFIG:Debug>>:${_release_dependency}>")
      endif()
      if(_SRT_LIBRARY_DEBUG)
        list(APPEND _SRT_PROPS_LINK_LIBRARIES
          "$<$<CONFIG:Debug>:${_debug_dependency}>")
      endif()
    endforeach()

    set_target_properties(
      SRT::SRT
      PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_SRT_PROPS_LINK_LIBRARIES}"
        INTERFACE_LINK_OPTIONS "${_SRT_STATIC_LINK_OPTIONS}"
    )
  endif()
endif()

mark_as_advanced(SRT_INCLUDE_DIR SRT_LIBRARY SRT_DLL)
