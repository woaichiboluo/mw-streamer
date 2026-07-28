add_executable(mw_streamer_e2e_runner
    runner/main.cc
)

target_link_libraries(mw_streamer_e2e_runner
    PRIVATE
        FFmpeg::avutil
        fmt::fmt
        ZLMediaKit::ToolKit
        mw::streamer
)
