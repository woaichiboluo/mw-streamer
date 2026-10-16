add_executable(mw_live_pipeline_tests live_pipeline_test.cc)
target_compile_definitions(mw_live_pipeline_tests PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_live_pipeline_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_live_pipeline_tests COMMAND mw_live_pipeline_tests)
set_tests_properties(mw_live_pipeline_tests PROPERTIES
    TIMEOUT 60 LABELS "pipeline;integration")
mw_copy_test_runtime_dlls(mw_live_pipeline_tests)

add_executable(mw_remux_pipeline_tests remux_pipeline_test.cc)
target_compile_definitions(mw_remux_pipeline_tests PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_remux_pipeline_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_remux_pipeline_tests COMMAND mw_remux_pipeline_tests)
set_tests_properties(mw_remux_pipeline_tests PROPERTIES
    TIMEOUT 60 LABELS "pipeline;integration")
mw_copy_test_runtime_dlls(mw_remux_pipeline_tests)

add_executable(mw_offline_pipeline_tests offline_pipeline_test.cc)
target_compile_definitions(mw_offline_pipeline_tests PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_offline_pipeline_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_offline_pipeline_tests COMMAND mw_offline_pipeline_tests)
set_tests_properties(mw_offline_pipeline_tests PROPERTIES
    TIMEOUT 60 LABELS "pipeline;integration")
mw_copy_test_runtime_dlls(mw_offline_pipeline_tests)
