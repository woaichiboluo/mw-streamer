cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_test(NAME mw_install_tests
        COMMAND "${CMAKE_COMMAND}"
            "-DMW_TEST_FRAME_CONVTER=${BUILD_FRAME_CONVTER}"
            "-DOpenCV_DIR=${OpenCV_DIR}"
            "-DCUDA_TOOLKIT_ROOT_DIR=${CUDA_TOOLKIT_ROOT_DIR}"
            "-DCUDAToolkit_ROOT=${CUDAToolkit_ROOT}"
            "-DMW_BUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DMW_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DMW_CONFIG=$<CONFIG>"
            "-DMW_GENERATOR=${CMAKE_GENERATOR}"
            "-DMW_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DMW_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DMW_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DMW_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            "-DFFMPEG_ROOT=${FFMPEG_ROOT}"
            "-DSRT_ROOT=${SRT_ROOT}"
            "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_install.cmake")
    set_tests_properties(mw_install_tests PROPERTIES TIMEOUT 180 LABELS install)
    return()
endif()

project(mw_install_consumer LANGUAGES CXX)
enable_testing()
set(module_path_before "${CMAKE_MODULE_PATH}")
if(MW_TEST_MISSING_DEPENDENCY)
    set(CMAKE_DISABLE_FIND_PACKAGE_FFmpeg TRUE)
    find_package(mw_streamer 0.1 CONFIG QUIET)
    if(mw_streamer_FOUND OR TARGET mw::streamer)
        message(FATAL_ERROR "The SDK was imported without its FFmpeg dependency")
    endif()
    if(NOT "${CMAKE_MODULE_PATH}" STREQUAL "${module_path_before}")
        message(FATAL_ERROR "The failed package lookup changed CMAKE_MODULE_PATH")
    endif()
    return()
endif()
find_package(mw_streamer 0.1 CONFIG REQUIRED)
if(NOT "${CMAKE_MODULE_PATH}" STREQUAL "${module_path_before}")
    message(FATAL_ERROR "The package lookup changed CMAKE_MODULE_PATH")
endif()

add_executable(mw_log_install_tests log_install_test.cc)
target_link_libraries(mw_log_install_tests PRIVATE mw::log)
add_test(NAME mw_log_install_tests COMMAND mw_log_install_tests)

add_executable(mw_streamer_install_tests streamer_install_test.cc)
target_link_libraries(mw_streamer_install_tests PRIVATE mw::streamer)
add_test(NAME mw_streamer_install_tests COMMAND mw_streamer_install_tests)

if(MW_TEST_FRAME_CONVTER)
    find_package(mw_frame_convter 0.1 CONFIG REQUIRED)
    if(NOT "${CMAKE_MODULE_PATH}" STREQUAL "${module_path_before}")
        message(FATAL_ERROR "The frame conversion package lookup changed CMAKE_MODULE_PATH")
    endif()
    add_executable(mw_frame_convter_install_tests frame_convter_install_test.cc)
    target_link_libraries(mw_frame_convter_install_tests PRIVATE mw::frame_convter)
    add_test(NAME mw_frame_convter_install_tests COMMAND mw_frame_convter_install_tests)
endif()
