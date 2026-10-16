add_executable(mw_recorder_tests recorder_test.cc)
target_compile_definitions(mw_recorder_tests PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_recorder_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_recorder_tests COMMAND mw_recorder_tests)
set_tests_properties(mw_recorder_tests PROPERTIES
    TIMEOUT 60 LABELS "recorder;integration")
mw_copy_test_runtime_dlls(mw_recorder_tests)
