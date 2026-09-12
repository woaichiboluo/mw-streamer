set(MW_STREAMER_SOURCES
    c_api.cc
    cache/packet_queue.cc
    config/toml.cc
    converter/av_packet_to_zlm_frame_converter.cc
    converter/internal/codec_bridge.cc
    converter/zlm_codec_parameters_converter.cc
    converter/zlm_packet_converter.cc
    decoder/audio_decoder.cc
    decoder/decoder_sink.cc
    decoder/internal/codec_finder.cc
    decoder/video_decoder.cc
    encoder/audio_encoder.cc
    encoder/encoder_sink.cc
    encoder/internal/options.cc
    encoder/video_encoder.cc
    ffmpeg/codec_context.cc
    ffmpeg/codec_parameters.cc
    ffmpeg/dictionary.cc
    ffmpeg/error.cc
    ffmpeg/frame.cc
    ffmpeg/hardware_context.cc
    ffmpeg/input_format_context.cc
    ffmpeg/packet.cc
    ffmpeg/pixel_format.cc
    ffmpeg/stream_info.cc
    init/init.cc
    input/file_input.cc
    input/internal/zlm_timestamp_reviser.cc
    input/player_proxy.cc
    input/zlm_input.cc
    log/logging.cc
    media/internal/codec_bridge.cc
    output/internal/remux_output.cc
    output/recording_target.cc
    output/remux_sink.cc
    performance/operation_recorder.cc
    performance/pipeline_snapshot.cc
    pipeline/pipeline.cc
    pipeline/pipeline_builder.cc
    processor/analysis_processor_sink.cc
    processor/internal/audio_frame_allocator.cc
    processor/internal/enum_converter.cc
    processor/internal/execution_context_adapter.cc
    processor/internal/frame_adapter.cc
    processor/internal/source_info_adapter.cc
    processor/internal/video_frame_allocator.cc
    processor/processor_handler.cc
    processor/transform_processor_sink.cc
    resampler/audio_resampler.cc
    sink/sink.cc
    synchronizer/internal/realtime_frame_scheduler.cc
    synchronizer/internal/standby_video_frame.cc
    synchronizer/synchronizer_sink.cc
    zlm/internal/config_validator.cc
)

if(BUILD_SHARED)
    add_library(mw_streamer SHARED ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer PRIVATE MW_STREAMER_BUILDING_LIBRARY)
else()
    add_library(mw_streamer STATIC ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer PUBLIC MW_STREAMER_STATIC_LIBRARY)
endif()
add_library(mw::streamer ALIAS mw_streamer)

target_link_libraries(mw_streamer
    PUBLIC
        ZLMediaKit::MediaKit
        ZLMediaKit::ToolKit
        SRT::SRT
        fmt::fmt
    PRIVATE
        HdrHistogram::HdrHistogram
        ZLMediaKit::ext-codec
        ZLMediaKit::SRT
        spdlog::spdlog
        tomlplusplus::tomlplusplus
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mw_streamer
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
    target_link_libraries(mw_streamer
        PUBLIC
            FFmpeg::avcodec
            FFmpeg::avformat
            FFmpeg::avutil
            FFmpeg::swresample
            FFmpeg::swscale
    )
endif()

target_include_directories(mw_streamer
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)
