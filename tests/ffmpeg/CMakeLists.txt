add_executable(mw_ffmpeg_error_tests error_test.cc)

target_link_libraries(mw_ffmpeg_error_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)

add_test(NAME mw_ffmpeg_error_tests COMMAND mw_ffmpeg_error_tests)
