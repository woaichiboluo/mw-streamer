add_executable(mw_av_encoder_tests av_encoder_test.cc)
target_link_libraries(mw_av_encoder_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avcodec fmt::fmt)
add_test(NAME mw_av_encoder_tests COMMAND mw_av_encoder_tests)
set_tests_properties(mw_av_encoder_tests PROPERTIES TIMEOUT 60)
