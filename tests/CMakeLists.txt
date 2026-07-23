add_executable(mw_logging_tests
    logging_test.cpp
)

target_link_libraries(mw_logging_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::SRT
        ZLMediaKit::ToolKit
        mw::streamer
)

include(Catch)
catch_discover_tests(mw_logging_tests)

add_executable(mw_init_tests
    init_test.cpp
)

target_link_libraries(mw_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer
)

catch_discover_tests(mw_init_tests)

add_executable(mw_srt_init_tests
    srt_init_test.cpp
)

target_link_libraries(mw_srt_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::SRT
        mw::streamer
)

catch_discover_tests(mw_srt_init_tests)
