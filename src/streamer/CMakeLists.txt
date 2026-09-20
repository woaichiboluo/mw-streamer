set(MW_STREAMER_CORE_SOURCES
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
    log/third_party_log_bridge.cc
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
    sink/frame_custom_sink.cc
    sink/packet_custom_sink.cc
    synchronizer/internal/realtime_frame_scheduler.cc
    synchronizer/internal/standby_video_frame.cc
    synchronizer/synchronizer_sink.cc
    zlm/internal/config_validator.cc
)

set(MW_STREAMER_PUBLIC_DEPENDENCIES
    ZLMediaKit::MediaKit
    ZLMediaKit::ToolKit
    SRT::SRT
    tomlplusplus::tomlplusplus
)

set(MW_STREAMER_PRIVATE_DEPENDENCIES
    mw::log
    fmt::fmt
    HdrHistogram::HdrHistogram
    ZLMediaKit::ext-codec
    ZLMediaKit::SRT
)

set(MW_STREAMER_FFMPEG_DEPENDENCIES
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swresample
    FFmpeg::swscale
)

add_library(mw_streamer_core_objects OBJECT ${MW_STREAMER_CORE_SOURCES})

target_link_libraries(mw_streamer_core_objects
    PRIVATE
        ${MW_STREAMER_PUBLIC_DEPENDENCIES}
        ${MW_STREAMER_PRIVATE_DEPENDENCIES}
        ${MW_STREAMER_FFMPEG_DEPENDENCIES}
)

target_include_directories(mw_streamer_core_objects
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/streamer"
)

if(BUILD_SHARED)
    add_library(mw_streamer SHARED
        c_api.cc
        $<TARGET_OBJECTS:mw_streamer_core_objects>
    )
    target_compile_definitions(mw_streamer PRIVATE MW_STREAMER_BUILDING_LIBRARY)

    set_target_properties(mw_streamer_core_objects PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
else()
    add_library(mw_streamer STATIC
        c_api.cc
        $<TARGET_OBJECTS:mw_streamer_core_objects>
    )
    target_compile_definitions(mw_streamer_core_objects
        PRIVATE MW_STREAMER_STATIC_LIBRARY
    )
    target_compile_definitions(mw_streamer PUBLIC MW_STREAMER_STATIC_LIBRARY)
endif()
add_library(mw::streamer ALIAS mw_streamer)

function(_mw_configure_streamer_target target)
    target_link_libraries(${target}
        PUBLIC
            ${MW_STREAMER_PUBLIC_DEPENDENCIES}
        PRIVATE
            ${MW_STREAMER_PRIVATE_DEPENDENCIES}
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${target}
            PUBLIC
                "-Wl,--no-as-needed"
                ${MW_STREAMER_FFMPEG_DEPENDENCIES}
                "-Wl,--as-needed"
        )
    else()
        target_link_libraries(${target}
            PUBLIC
                ${MW_STREAMER_FFMPEG_DEPENDENCIES}
        )
    endif()

    target_include_directories(${target}
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/streamer"
    )
endfunction()

_mw_configure_streamer_target(mw_streamer)

if(BUILD_SHARED AND BUILD_TESTS)
    # Internal tests exercise implementation details that are intentionally
    # hidden from the shared library ABI. Reuse the production object files so
    # those tests do not require exporting private C++ symbols.
    add_library(mw_streamer_test STATIC
        $<TARGET_OBJECTS:mw_streamer_core_objects>
    )
    target_compile_definitions(mw_streamer_test
        PUBLIC MW_STREAMER_STATIC_LIBRARY
    )
    _mw_configure_streamer_target(mw_streamer_test)
endif()

if(BUILD_SHARED)
    set_target_properties(mw_streamer PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endif()
