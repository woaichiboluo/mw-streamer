set(MW_AV_INPUT_SOURCES
    ../streamer/ffmpeg/av_dictionary.cc
    ../streamer/ffmpeg/codec_parameters.cc
    ../streamer/ffmpeg/error.cc
    ../streamer/ffmpeg/packet.cc
    ../streamer/ffmpeg/stream_info.cc
    ../streamer/input/av_input.cc
    ../streamer/input/av_input_proxy.cc
)

if(BUILD_SHARED)
    add_library(mw_av_input SHARED ${MW_AV_INPUT_SOURCES})
    target_compile_definitions(mw_av_input PRIVATE MW_STREAMER_BUILDING_LIBRARY)
    set_target_properties(mw_av_input PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
else()
    add_library(mw_av_input STATIC ${MW_AV_INPUT_SOURCES})
    target_compile_definitions(mw_av_input PUBLIC MW_STREAMER_STATIC_LIBRARY)
endif()
add_library(mw::av_input ALIAS mw_av_input)
target_include_directories(mw_av_input PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(mw_av_input
    PUBLIC FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
    PRIVATE mw::log Threads::Threads
)
