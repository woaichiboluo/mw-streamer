add_executable(mw_scheduler_tests scheduler_source_test.cc scheduler_test_main.cc)
target_compile_definitions(mw_scheduler_tests PRIVATE
    MW_STREAMER_SCHEDULER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_scheduler_tests PRIVATE
    Catch2::Catch2 mw::streamer FFmpeg::avutil FFmpeg::avformat fmt::fmt)
add_test(NAME mw_scheduler_tests COMMAND mw_scheduler_tests)
set_tests_properties(mw_scheduler_tests PROPERTIES TIMEOUT 60)
