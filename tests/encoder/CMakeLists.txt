add_executable(mw_encoder_tests encoder_test.cc encoder_timing_test.cc
    encoder_gpu_test.cc)
target_compile_definitions(mw_encoder_tests PRIVATE
    MW_STREAMER_ENCODER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_encoder_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat fmt::fmt)
add_test(NAME mw_encoder_tests COMMAND mw_encoder_tests)
set_tests_properties(mw_encoder_tests PROPERTIES TIMEOUT 60)

add_executable(mw_encoder_record_probe encoder_record_probe.cc)
target_link_libraries(mw_encoder_record_probe PRIVATE
    mw::streamer FFmpeg::avformat fmt::fmt)
