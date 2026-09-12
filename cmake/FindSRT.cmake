# Locate SRT and provide SRT::SRT. Static packages must provide their private
# dependencies through srt.pc or, for official Windows packages, libsrt.props.

include(FindPackageHandleStandardArgs)

set(SRT_ROOT "" CACHE PATH "SRT installation prefix")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SRT QUIET srt)
endif()

find_path(SRT_INCLUDE_DIR NAMES srt/srt.h
  HINTS "${SRT_ROOT}" ${PC_SRT_INCLUDEDIR} ${PC_SRT_INCLUDE_DIRS}
  PATH_SUFFIXES include)

set(SRT_VERSION "${PC_SRT_VERSION}")
if(NOT SRT_VERSION AND SRT_INCLUDE_DIR)
  set(_SRT_VERSION_HEADER "${SRT_INCLUDE_DIR}/srt/version.h")
  if(EXISTS "${_SRT_VERSION_HEADER}")
    file(STRINGS "${_SRT_VERSION_HEADER}" _SRT_VERSION_LINE
      REGEX "^#define[ \t]+SRT_VERSION_STRING[ \t]+\"[^\"]+\"")
    string(REGEX REPLACE ".*SRT_VERSION_STRING[ \t]+\"([^\"]+)\".*"
      "\\1" SRT_VERSION "${_SRT_VERSION_LINE}")
  endif()
endif()

function(_srt_find_file output)
  cmake_parse_arguments(ARG "" "" "NAMES;HINTS;PATH_SUFFIXES" ${ARGN})
  unset(_file)
  find_file(_file NAMES ${ARG_NAMES} HINTS ${ARG_HINTS}
    PATH_SUFFIXES ${ARG_PATH_SUFFIXES} NO_CACHE)
  set(${output} "${_file}" PARENT_SCOPE)
endfunction()

function(_srt_path_is_in_directories output path)
  file(REAL_PATH "${path}" _path)
  set(_found FALSE)
  foreach(_directory IN LISTS ARGN)
    if(_directory)
      file(REAL_PATH "${_directory}" _directory)
      string(FIND "${_path}" "${_directory}/" _position)
      if(_position EQUAL 0)
        set(_found TRUE)
        break()
      endif()
    endif()
  endforeach()
  set(${output} ${_found} PARENT_SCOPE)
endfunction()

function(_srt_parse_windows_props props_file output_dependencies output_options)
  file(READ "${props_file}" _contents)
  string(REGEX MATCH "<AdditionalDependencies>([^<]+)</AdditionalDependencies>"
    _match "${_contents}")
  set(_dependencies "${CMAKE_MATCH_1}")
  string(REPLACE "%(AdditionalDependencies)" "" _dependencies
    "${_dependencies}")
  list(FILTER _dependencies EXCLUDE REGEX "^$")
  string(REGEX MATCH "<AdditionalOptions>([^<]+)</AdditionalOptions>"
    _match "${_contents}")
  set(_options "${CMAKE_MATCH_1}")
  string(REPLACE "%(AdditionalOptions)" "" _options "${_options}")
  separate_arguments(_options WINDOWS_COMMAND "${_options}")
  set(${output_dependencies} "${_dependencies}" PARENT_SCOPE)
  set(${output_options} "${_options}" PARENT_SCOPE)
endfunction()

set(_SRT_IS_STATIC FALSE)
set(_SRT_PROPS_FILE)
get_filename_component(_SRT_INCLUDE_PARENT "${SRT_INCLUDE_DIR}" DIRECTORY)
if(WIN32)
  # A .lib is ambiguous. Prefer a complete DLL/import-library pair, then
  # accept a static library.
  _srt_find_file(_SRT_SHARED_LIBRARY NAMES srt.lib libsrt.dll.a
    HINTS "${SRT_ROOT}" "${_SRT_INCLUDE_PARENT}" ${PC_SRT_LIBRARY_DIRS}
    PATH_SUFFIXES lib lib64 lib/Release-x64)
  _srt_find_file(_SRT_SHARED_DLL NAMES srt.dll libsrt.dll
    HINTS "${SRT_ROOT}" "${_SRT_INCLUDE_PARENT}"
    PATH_SUFFIXES bin bin/Release-x64)
  if(_SRT_SHARED_LIBRARY AND _SRT_SHARED_DLL)
    set(SRT_LIBRARY "${_SRT_SHARED_LIBRARY}" CACHE FILEPATH "SRT library" FORCE)
    set(SRT_DLL "${_SRT_SHARED_DLL}" CACHE FILEPATH "SRT DLL" FORCE)
  else()
    _srt_find_file(_SRT_STATIC_LIBRARY NAMES srt_static.lib srt.lib libsrt.a
      HINTS "${SRT_ROOT}" "${_SRT_INCLUDE_PARENT}" ${PC_SRT_LIBRARY_DIRS}
      PATH_SUFFIXES lib lib64 lib/Release-x64)
    set(SRT_LIBRARY "${_SRT_STATIC_LIBRARY}" CACHE FILEPATH "SRT library" FORCE)
    set(SRT_DLL "" CACHE FILEPATH "SRT DLL" FORCE)
    if(SRT_LIBRARY)
      set(_SRT_IS_STATIC TRUE)
    endif()
  endif()
  _srt_find_file(_SRT_PROPS_FILE NAMES libsrt.props
    HINTS "${SRT_ROOT}" "${_SRT_INCLUDE_PARENT}")
else()
  find_library(SRT_LIBRARY NAMES srt HINTS "${SRT_ROOT}" ${PC_SRT_LIBDIR}
    ${PC_SRT_LIBRARY_DIRS} PATH_SUFFIXES lib lib64)
  if(SRT_LIBRARY MATCHES "${CMAKE_STATIC_LIBRARY_SUFFIX}$")
    set(_SRT_IS_STATIC TRUE)
  endif()
endif()

set(_SRT_DEPENDENCIES_FOUND TRUE)
if(_SRT_IS_STATIC AND SRT_LIBRARY)
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
if(WIN32 AND NOT _SRT_IS_STATIC)
  list(APPEND _SRT_REQUIRED_VARS SRT_DLL)
endif()
if(_SRT_IS_STATIC)
  list(APPEND _SRT_REQUIRED_VARS _SRT_DEPENDENCIES_FOUND)
endif()
find_package_handle_standard_args(SRT REQUIRED_VARS ${_SRT_REQUIRED_VARS}
  VERSION_VAR SRT_VERSION
  REASON_FAILURE_MESSAGE
    "Static SRT requires matching srt.pc metadata or the official Windows libsrt.props package")

set(SRT_INCLUDE_DIRS "${SRT_INCLUDE_DIR}")
set(SRT_LIBRARIES "${SRT_LIBRARY}")
set(SRT_DEFINITIONS "${PC_SRT_CFLAGS_OTHER}")
get_filename_component(SRT_LIBRARY_DIRS "${SRT_LIBRARY}" DIRECTORY)

if(SRT_FOUND AND NOT TARGET SRT::SRT)
  add_library(SRT::SRT UNKNOWN IMPORTED GLOBAL)
  set_target_properties(SRT::SRT PROPERTIES
    INTERFACE_COMPILE_OPTIONS "${PC_SRT_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}")
  if(WIN32 AND NOT _SRT_IS_STATIC)
    set_target_properties(SRT::SRT PROPERTIES
      IMPORTED_IMPLIB "${SRT_LIBRARY}"
      IMPORTED_LOCATION "${SRT_DLL}"
      INTERFACE_COMPILE_DEFINITIONS SRT_DYNAMIC
      INTERFACE_LINK_LIBRARIES Ws2_32)
    list(APPEND SRT_DEFINITIONS SRT_DYNAMIC)
  else()
    set_property(TARGET SRT::SRT PROPERTY IMPORTED_LOCATION "${SRT_LIBRARY}")
  endif()

  if(_SRT_IS_STATIC AND PC_SRT_FOUND AND _SRT_PC_MATCHES_LIBRARY)
    set_target_properties(SRT::SRT PROPERTIES
      INTERFACE_COMPILE_OPTIONS "${_SRT_STATIC_COMPILE_OPTIONS}"
      INTERFACE_LINK_DIRECTORIES "${_SRT_STATIC_LIBRARY_DIRS}"
      INTERFACE_LINK_LIBRARIES "${_SRT_STATIC_DEPENDENCIES}"
      INTERFACE_LINK_OPTIONS "${_SRT_STATIC_LINK_OPTIONS}")
  elseif(_SRT_IS_STATIC AND WIN32 AND _SRT_PROPS_FILE)
    set(_SRT_PROPS_LINK_LIBRARIES)
    get_filename_component(_SRT_LIBRARY_DIR "${SRT_LIBRARY}" DIRECTORY)
    foreach(_dependency IN LISTS _SRT_PROPS_DEPENDENCIES)
      string(STRIP "${_dependency}" _dependency)
      if(NOT _dependency OR _dependency STREQUAL "srt.lib")
        continue()
      endif()
      if(_dependency STREQUAL "libssl.lib" AND TARGET OpenSSL::SSL)
        list(APPEND _SRT_PROPS_LINK_LIBRARIES OpenSSL::SSL)
      elseif(_dependency STREQUAL "libcrypto.lib" AND TARGET OpenSSL::Crypto)
        list(APPEND _SRT_PROPS_LINK_LIBRARIES OpenSSL::Crypto)
      elseif(_dependency STREQUAL "crypt32.lib" OR _dependency STREQUAL "ws2_32.lib")
        list(APPEND _SRT_PROPS_LINK_LIBRARIES "${_dependency}")
      else()
        set(_dependency_path "${_SRT_LIBRARY_DIR}/${_dependency}")
        if(NOT EXISTS "${_dependency_path}")
          message(FATAL_ERROR "SRT static dependency does not exist: ${_dependency_path}")
        endif()
        list(APPEND _SRT_PROPS_LINK_LIBRARIES "${_dependency_path}")
      endif()
    endforeach()
    set_target_properties(SRT::SRT PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_SRT_PROPS_LINK_LIBRARIES}"
      INTERFACE_LINK_OPTIONS "${_SRT_STATIC_LINK_OPTIONS}")
  endif()
endif()

mark_as_advanced(SRT_INCLUDE_DIR SRT_LIBRARY SRT_DLL)
