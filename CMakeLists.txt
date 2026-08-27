cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)

project(mw_streamer VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTS "Build mw-streamer tests" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_EXAMPLES "Build mw-streamer examples" OFF)
option(BUILD_OPENCV_ADAPTER "Build the optional OpenCV adapter" OFF)
option(BUILD_SHARED_LIBS "Build mw-streamer as a shared library" ON)

set(_MW_STREAMER_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)

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
find_package(SRT REQUIRED)

add_subdirectory(third_party/ZLMediaKit)
add_subdirectory(third_party/HdrHistogram_c)

set(BUILD_SHARED_LIBS ${_MW_STREAMER_BUILD_SHARED_LIBS})
unset(_MW_STREAMER_BUILD_SHARED_LIBS)

set(MW_STREAMER_CORE_SOURCES
    src/cache/packet_queue.cc
    src/config/toml.cc
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
    src/ffmpeg/input_format_context.cc
    src/ffmpeg/packet.cc
    src/ffmpeg/pixel_format.cc
    src/ffmpeg/stream_info.cc
    src/init/init.cc
    src/input/internal/zlm_timestamp_reviser.cc
    src/input/player_proxy.cc
    src/log/logging.cc
    src/media/internal/codec_bridge.cc
    src/output/internal/output_sink_worker.cc
    src/output/output_sink.cc
    src/output/output_session.cc
    src/output/recording_target.cc
    src/performance/internal/local_file_collector.cc
    src/performance/internal/remux_collector.cc
    src/performance/internal/stage_recorder.cc
    src/performance/internal/streaming_collector.cc
    src/pipeline/file_pipeline.cc
    src/pipeline/internal/remux/source_output_worker.cc
    src/pipeline/internal/streaming/audio_processing_chain.cc
    src/pipeline/internal/streaming/audio_worker.cc
    src/pipeline/internal/streaming/encoded_output_sink.cc
    src/pipeline/internal/streaming/frame_synchronizer.cc
    src/pipeline/internal/streaming/output_event_mailbox.cc
    src/pipeline/internal/streaming/output_worker.cc
    src/pipeline/internal/streaming/standby_video_frame.cc
    src/pipeline/internal/streaming/video_worker.cc
    src/pipeline/internal/streaming/video_processing_chain.cc
    src/pipeline/remux_pipeline.cc
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

add_library(mw_streamer
    src/c_api.cc
    ${MW_STREAMER_CORE_SOURCES}
)
add_library(mw::streamer ALIAS mw_streamer)

function(mw_configure_static_interface target)
    target_compile_definitions(${target}
        PUBLIC MW_STREAMER_STATIC_LIBRARY
    )
    target_link_libraries(${target}
        PUBLIC
            ZLMediaKit::MediaKit
            ZLMediaKit::ToolKit
            fmt::fmt
        PRIVATE
            SRT::SRT
            HdrHistogram::HdrHistogram
            ZLMediaKit::ext-codec
            ZLMediaKit::SRT
            spdlog::spdlog
            tomlplusplus::tomlplusplus
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${target}
            PUBLIC
                "-Wl,--no-as-needed"
                FFmpeg::avcodec
                FFmpeg::avformat
                FFmpeg::avutil
                FFmpeg::swresample
                FFmpeg::swscale
                "-Wl,--as-needed"
        )
    else()
        target_link_libraries(${target}
            PUBLIC
                FFmpeg::avcodec
                FFmpeg::avformat
                FFmpeg::avutil
                FFmpeg::swresample
                FFmpeg::swscale
        )
    endif()
endfunction()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(mw_streamer
        PRIVATE MW_STREAMER_BUILDING_LIBRARY
    )
    set_target_properties(mw_streamer
        PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(mw_streamer
            PRIVATE
                "LINKER:--version-script=${PROJECT_SOURCE_DIR}/cmake/mw_streamer.map"
                "LINKER:--exclude-libs,ALL"
        )
    endif()
else()
    mw_configure_static_interface(mw_streamer)
endif()

target_include_directories(mw_streamer
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

if(BUILD_SHARED_LIBS)
    target_link_libraries(mw_streamer
        PRIVATE
            SRT::SRT
            HdrHistogram::HdrHistogram
            ZLMediaKit::MediaKit
            ZLMediaKit::ToolKit
            ZLMediaKit::ext-codec
            ZLMediaKit::SRT
            fmt::fmt
            spdlog::spdlog
            tomlplusplus::tomlplusplus
    )
endif()

if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mw_streamer
        PRIVATE
            "-Wl,--no-as-needed"
            FFmpeg::avcodec
            FFmpeg::avformat
            FFmpeg::avutil
            FFmpeg::swresample
            FFmpeg::swscale
            "-Wl,--as-needed"
    )
elseif(BUILD_SHARED_LIBS)
    target_link_libraries(mw_streamer
        PRIVATE
            FFmpeg::avcodec
            FFmpeg::avformat
            FFmpeg::avutil
            FFmpeg::swresample
            FFmpeg::swscale
    )
endif()

if(BUILD_OPENCV_ADAPTER)
    add_subdirectory(adapters/opencv)
endif()

if(BUILD_TESTS)
    enable_testing()

    if(BUILD_SHARED_LIBS)
        add_library(mw_streamer_test_support STATIC
            ${MW_STREAMER_CORE_SOURCES}
        )
        add_library(mw::streamer_test ALIAS mw_streamer_test_support)
        mw_configure_static_interface(mw_streamer_test_support)
        target_include_directories(mw_streamer_test_support
            PUBLIC
                "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src"
        )
    else()
        add_library(mw::streamer_test ALIAS mw_streamer)
    endif()

    add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
