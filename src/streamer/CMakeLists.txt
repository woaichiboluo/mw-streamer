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
    remuxer/packet_converter.cc
    remuxer/async_remuxer.cc
    remuxer/remux_output.cc
    remuxer/sync_remuxer.cc
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

set(MW_STREAMER_TARGETS mw_streamer)
if(BUILD_TESTS AND BUILD_SHARED)
    # Fixtures using ZLM internals must share the SDK owned by Init/Shutdown.
    add_library(mw_streamer_test_static STATIC EXCLUDE_FROM_ALL ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer_test_static PUBLIC MW_STREAMER_STATIC_LIBRARY)
    list(APPEND MW_STREAMER_TARGETS mw_streamer_test_static)
endif()

foreach(streamer_target IN LISTS MW_STREAMER_TARGETS)
    target_include_directories(${streamer_target}
        PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    )

    target_link_libraries(${streamer_target}
        PUBLIC
            FFmpeg::avcodec
            FFmpeg::avutil
            mw::log
        PRIVATE
            ZLMediaKit::MediaKit
            FFmpeg::avformat
            FFmpeg::swresample
            Threads::Threads
            SRT::SRT
            fmt::fmt
            ZLMediaKit::ext-codec
    )

    if(WIN32)
        target_link_libraries(${streamer_target} PRIVATE winmm avrt)
    endif()
endforeach()
