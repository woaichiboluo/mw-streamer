add_executable(mw_ffmpeg_error_tests error_test.cc)

target_link_libraries(mw_ffmpeg_error_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)

add_test(NAME mw_ffmpeg_error_tests COMMAND mw_ffmpeg_error_tests)

add_executable(mw_ffmpeg_hw_device_tests hw_device_context_test.cc)
target_link_libraries(mw_ffmpeg_hw_device_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)
add_test(NAME mw_ffmpeg_hw_device_tests COMMAND mw_ffmpeg_hw_device_tests)

add_executable(mw_ffmpeg_stream_info_tests stream_info_test.cc)
target_link_libraries(mw_ffmpeg_stream_info_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_ffmpeg_stream_info_tests COMMAND mw_ffmpeg_stream_info_tests)
