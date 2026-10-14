if(BUILD_SHARED)
    add_library(mw_log SHARED log/logging.cc)
    target_compile_definitions(mw_log PRIVATE MW_LOG_BUILDING_LIBRARY)
else()
    add_library(mw_log STATIC log/logging.cc)
    target_compile_definitions(mw_log PUBLIC MW_LOG_STATIC_LIBRARY)
endif()
add_library(mw::log ALIAS mw_log)

target_include_directories(mw_log
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)

target_link_libraries(mw_log
    PUBLIC
        fmt::fmt
    PRIVATE
        spdlog::spdlog
)

target_compile_features(mw_log PUBLIC cxx_std_17)
set_target_properties(mw_log PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

set(MW_STREAMER_SOURCES
    streamer/ffmpeg/av_dictionary.cc
    streamer/ffmpeg/codec_context.cc
    streamer/ffmpeg/codec_parameters.cc
    streamer/ffmpeg/error.cc
    streamer/ffmpeg/filter_graph.cc
    streamer/ffmpeg/frame.cc
    streamer/ffmpeg/hw_device_context.cc
    streamer/ffmpeg/packet.cc
    streamer/ffmpeg/stream_info.cc
    streamer/init/init.cc
    streamer/input/av_input.cc
    streamer/input/av_input_proxy.cc
    streamer/decoder/av_decoder.cc
    streamer/input/av_frame_source.cc
    streamer/input/av_media_input.cc
    streamer/output/output.cc
    streamer/output/encode_push_sink.cc
    streamer/platform/thread.cc
    streamer/scheduler/scheduler.cc
    streamer/encoder/av_encoder.cc
    streamer/performance/remuxer_performance.cc
    streamer/remuxer/packet_converter.cc
    streamer/remuxer/remux_output.cc
    streamer/remuxer/remuxer.cc
)

if(WIN32)
    list(APPEND MW_STREAMER_SOURCES streamer/platform/runtime_windows.cc)
else()
    list(APPEND MW_STREAMER_SOURCES streamer/platform/runtime_posix.cc)
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
target_include_directories(mw_streamer PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(mw_streamer
    PUBLIC mw::log FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::avfilter
    PRIVATE fmt::fmt Threads::Threads TBB::tbb FFmpeg::avdevice
        ZLMediaKit::MediaKit ZLMediaKit::ext-codec
)
if(WIN32)
    target_link_libraries(mw_streamer PRIVATE winmm)
endif()

if(BUILD_TESTS AND BUILD_SHARED)
    # Internal SDK tests keep one SDK/runtime instance within their executable.
    add_library(mw_streamer_test_static STATIC ${MW_STREAMER_SOURCES})
    target_compile_definitions(mw_streamer_test_static PUBLIC MW_STREAMER_STATIC_LIBRARY)
    target_include_directories(mw_streamer_test_static PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
    target_link_libraries(mw_streamer_test_static PUBLIC
        mw::log FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::avfilter
        PRIVATE fmt::fmt Threads::Threads TBB::tbb FFmpeg::avdevice
            ZLMediaKit::MediaKit ZLMediaKit::ext-codec)
    if(WIN32)
        target_link_libraries(mw_streamer_test_static PRIVATE winmm)
    endif()
endif()
