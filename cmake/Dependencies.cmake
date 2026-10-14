include_guard(GLOBAL)

include(FetchContent)

if(NOT TARGET fmt::fmt)
    set(FMT_DOC OFF CACHE BOOL "Build fmt documentation" FORCE)
    set(FMT_TEST OFF CACHE BOOL "Build fmt tests" FORCE)
    set(FMT_FUZZ OFF CACHE BOOL "Build fmt fuzzers" FORCE)
    set(FMT_CUDA_TEST OFF CACHE BOOL "Build fmt CUDA tests" FORCE)
    set(FMT_INSTALL OFF CACHE BOOL "Generate fmt install targets" FORCE)
    set(FMT_MODULE OFF CACHE BOOL "Build fmt as a C++ module" FORCE)

    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 407c905e45ad75fc29bf0f9bb7c5c2fd3475976f # 12.1.0
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(fmt)
endif()

if(NOT TARGET ZLMediaKit::MediaKit)
    find_package(OpenSSL REQUIRED)
    find_package(SRT REQUIRED)
    set(ENABLE_MSVC_MT OFF CACHE BOOL "Use the DLL MSVC runtime in ZLMediaKit" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/ZLMediaKit"
        "${PROJECT_BINARY_DIR}/third_party/ZLMediaKit")
    target_link_libraries(ZLToolKit PUBLIC mw::log)

    # Treat SDK headers as external while keeping first-party warnings strict.
    foreach(sdk_target zlmediakit ext-codec ZLToolKit mov flv mpeg zlmediakit_srt_adapter)
        get_target_property(sdk_includes ${sdk_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(sdk_includes)
            set_property(TARGET ${sdk_target} APPEND PROPERTY
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${sdk_includes}")
        endif()
    endforeach()
endif()

function(mw_copy_test_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endfunction()

if(NOT TARGET spdlog::spdlog)
    set(SPDLOG_BUILD_ALL OFF CACHE BOOL "Build all spdlog artifacts" FORCE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build shared spdlog library" FORCE)
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use external fmt library" FORCE)
    set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "Use external header-only fmt library" FORCE)
    set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "Use std::format instead of fmt" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
    set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "Build spdlog header-only examples" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
    set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "Build spdlog header-only tests" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Build spdlog benchmarks" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "Generate spdlog install targets" FORCE)

    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d # 1.17.0
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

if(BUILD_TESTS AND NOT TARGET Catch2::Catch2WithMain)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Install Catch2 documentation" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Install Catch2 extras" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "Build Catch2 self-tests" FORCE)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()
