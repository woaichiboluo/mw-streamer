set(MW_STREAMER_SOURCES
    ffmpeg/codec_context.cc
    ffmpeg/codec_parameters.cc
    ffmpeg/decoder.cc
    ffmpeg/encoder.cc
    ffmpeg/error.cc
    ffmpeg/frame.cc
    ffmpeg/hw_device_context.cc
    ffmpeg/packet.cc
    ffmpeg/stream_info.cc
    encoder/encoder.cc
    init/init.cc
    input/ffmpeg_input.cc
    performance/performance.cc
    processor/processor.cc
    scheduler/scheduler.cc
)

if(WIN32)
    list(APPEND MW_STREAMER_SOURCES platform/platform_windows.cc)
else()
    list(APPEND MW_STREAMER_SOURCES platform/platform_posix.cc)
endif()

if(BUILD_SHARED)
    add_library(mw_streamer SHARED ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer PRIVATE MW_STREAMER_BUILDING_LIBRARY)
    set_target_properties(mw_streamer PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
else()
    add_library(mw_streamer STATIC ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer PUBLIC MW_STREAMER_STATIC_LIBRARY)
endif()
add_library(mw::streamer ALIAS mw_streamer)

target_include_directories(mw_streamer
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)

target_link_libraries(mw_streamer
    PUBLIC
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::MediaKit
    PRIVATE
        mw::log
        FFmpeg::avformat
        FFmpeg::swresample
        Threads::Threads
        SRT::SRT
        fmt::fmt
        ZLMediaKit::ext-codec
)

if(WIN32)
    target_link_libraries(mw_streamer PRIVATE winmm avrt)
endif()
