cmake_minimum_required(VERSION 3.19)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(MW_STREAMER_IS_TOP_LEVEL ON)
else()
  set(MW_STREAMER_IS_TOP_LEVEL OFF)
endif()

option(MW_STREAMER_BUILD_EXAMPLES "Build mw-streamer examples" ${MW_STREAMER_IS_TOP_LEVEL})
option(MW_STREAMER_BUILD_TESTS "Build mw-streamer tests" ${MW_STREAMER_IS_TOP_LEVEL})

include(FetchContent)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(
  FFmpeg
  REQUIRED
  COMPONENTS AVCODEC AVFORMAT AVUTIL SWRESAMPLE SWSCALE
)

set(FMT_DOC OFF CACHE BOOL "Build fmt documentation" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "Generate fmt install target" FORCE)
set(FMT_TEST OFF CACHE BOOL "Build fmt tests" FORCE)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 12.1.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog shared library" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Build spdlog benchmarks" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Generate spdlog install target" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use the external fmt library" FORCE)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

add_library(
  mw_streamer STATIC
  src/ffmpeg/buffer_ref.cpp
  src/ffmpeg/channel_layout.cpp
  src/ffmpeg/codec_context.cpp
  src/ffmpeg/codec_parameters.cpp
  src/ffmpeg/dictionary.cpp
  src/ffmpeg/frame.cpp
  src/ffmpeg/input_format_context.cpp
  src/ffmpeg/output_format_context.cpp
  src/ffmpeg/packet.cpp
  src/ffmpeg/swr_context.cpp
  src/internal/aac_encoder.cpp
  src/internal/aligned_audio_buffer.cpp
  src/internal/audio_auto_decoder.cpp
  src/internal/audio_converter.cpp
  src/internal/audio_fifo.cpp
  src/internal/common/error.cpp
  src/internal/common/ffmpeg_log.cpp
  src/internal/common/ffmpeg_interrupt.cpp
  src/internal/common/reconnect_backoff.cpp
  src/internal/common/timestamp.cpp
  src/internal/compressed_packet_delay_buffer.cpp
  src/internal/cuda_device_context.cpp
  src/internal/demuxer.cpp
  src/internal/fragmented_mp4_muxer.cpp
  src/internal/gpu_frame_utils.cpp
  src/internal/gpu_nv12_frame_pool.cpp
  src/internal/input_mode.cpp
  src/internal/input_options.cpp
  src/internal/network_muxer.cpp
  src/internal/nv_decoder.cpp
  src/internal/nv_encoder.cpp
  src/internal/output_worker.cpp
  src/internal/pipeline_config_validator.cpp
  src/internal/processor_context.cpp
  src/internal/standby_image.cpp
  src/internal/video_color.cpp
  src/internal/video_synchronizer.cpp
  src/processor/processor.cpp
  src/pipeline.cpp
  src/status.cpp
  src/version.cpp
)
add_library(mw::streamer ALIAS mw_streamer)

target_compile_features(mw_streamer PUBLIC cxx_std_17)
target_compile_definitions(mw_streamer PRIVATE MW_STREAMER_VERSION="${PROJECT_VERSION}")
target_include_directories(
  mw_streamer
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(
  mw_streamer
  PRIVATE
    FFmpeg::FFmpeg
    fmt::fmt
    spdlog::spdlog
)

if(MW_STREAMER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MW_STREAMER_BUILD_TESTS)
  enable_testing()

  set(BUILD_GMOCK ON CACHE BOOL "Build GoogleMock" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "Generate GoogleTest install target" FORCE)
  if(MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
  endif()

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)

  add_subdirectory(tests)
endif()
