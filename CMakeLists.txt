cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

include(CTest)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(cmake/Dependencies.cmake)

find_package(FFmpeg REQUIRED COMPONENTS
    AVCODEC
    AVFORMAT
    AVUTIL
    SWRESAMPLE
    SWSCALE
)
find_package(CUDAToolkit REQUIRED)
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit)
add_subdirectory(third_party/HdrHistogram_c)

add_library(mw_streamer STATIC
    src/cache/packet_queue.cc
    src/converter/av_packet_to_zlm_frame_converter.cc
    src/converter/internal/codec_bridge.cc
    src/converter/zlm_codec_parameters_converter.cc
    src/converter/zlm_packet_converter.cc
    src/decoder/audio_decoder.cc
    src/decoder/internal/codec_finder.cc
    src/decoder/video_decoder.cc
    src/encoder/audio_encoder.cc
    src/encoder/internal/options.cc
    src/encoder/video_encoder.cc
    src/ffmpeg/codec_context.cc
    src/ffmpeg/codec_parameters.cc
    src/ffmpeg/dictionary.cc
    src/ffmpeg/error.cc
    src/ffmpeg/frame.cc
    src/ffmpeg/hardware_context.cc
    src/ffmpeg/packet.cc
    src/ffmpeg/pixel_format.cc
    src/ffmpeg/stream_info.cc
    src/init/init.cc
    src/input/internal/zlm_timestamp_reviser.cc
    src/input/player_proxy.cc
    src/log/logging.cc
    src/media/internal/codec_bridge.cc
    src/output/output_session.cc
    src/output/recording_target.cc
    src/performance/internal/stage_recorder.cc
    src/performance/internal/streaming_collector.cc
    src/pipeline/internal/streaming/audio_processing_chain.cc
    src/pipeline/internal/streaming/audio_worker.cc
    src/pipeline/internal/streaming/frame_synchronizer.cc
    src/pipeline/internal/streaming/output_worker.cc
    src/pipeline/internal/streaming/standby_video_frame.cc
    src/pipeline/internal/streaming/video_worker.cc
    src/pipeline/internal/streaming/video_processing_chain.cc
    src/pipeline/streaming_pipeline.cc
    src/processor/internal/audio_frame_allocator.cc
    src/processor/internal/enum_converter.cc
    src/processor/internal/execution_context_adapter.cc
    src/processor/internal/frame_adapter.cc
    src/processor/internal/source_info_adapter.cc
    src/processor/internal/video_frame_allocator.cc
    src/processor/file_processor_handler.cc
    src/processor/processor_handler.cc
    src/processor/streaming_processor_handler.cc
    src/resampler/audio_resampler.cc
    src/zlm/internal/config_validator.cc
)
add_library(mw::streamer ALIAS mw_streamer)

target_include_directories(mw_streamer
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(mw_streamer
    PUBLIC
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::MediaKit
        ZLMediaKit::ToolKit
        fmt::fmt
    PRIVATE
        SRT::SRT
        CUDA::cuda_driver
        HdrHistogram::HdrHistogram
        ZLMediaKit::ext-codec
        ZLMediaKit::SRT
        spdlog::spdlog
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mw_streamer
        PRIVATE
            "-Wl,--push-state,--no-as-needed"
            FFmpeg::avformat
            FFmpeg::swresample
            FFmpeg::swscale
            "-Wl,--pop-state"
    )
else()
    target_link_libraries(mw_streamer
        PRIVATE
            FFmpeg::avformat
            FFmpeg::swresample
            FFmpeg::swscale
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
