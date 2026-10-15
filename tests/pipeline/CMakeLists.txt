add_executable(mw_single_live_pipeline_tests single_live_pipeline_test.cc)
target_compile_definitions(mw_single_live_pipeline_tests PRIVATE
    MW_SINGLE_LIVE_PIPELINE_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
target_link_libraries(mw_single_live_pipeline_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::avformat)
add_test(NAME mw_single_live_pipeline_tests COMMAND mw_single_live_pipeline_tests)
set_tests_properties(mw_single_live_pipeline_tests PROPERTIES
    TIMEOUT 90 LABELS "pipeline")
mw_copy_test_runtime_dlls(mw_single_live_pipeline_tests)
