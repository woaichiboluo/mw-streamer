add_executable(mw_remux_pipeline_tests remux_pipeline_test.cc)
target_compile_definitions(mw_remux_pipeline_tests PRIVATE
    MW_STREAMER_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_remux_pipeline_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_remux_pipeline_tests COMMAND mw_remux_pipeline_tests)
set_tests_properties(mw_remux_pipeline_tests PROPERTIES
    TIMEOUT 60 LABELS "pipeline")

add_executable(mw_remux_pipeline_backpressure_tests
    remux_pipeline_backpressure_test.cc)
target_compile_definitions(mw_remux_pipeline_backpressure_tests PRIVATE
    MW_STREAMER_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_remux_pipeline_backpressure_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat ZLMediaKit::MediaKit)
if(BUILD_SHARED)
    target_link_libraries(mw_remux_pipeline_backpressure_tests PRIVATE
        mw_streamer_test_static)
else()
    target_link_libraries(mw_remux_pipeline_backpressure_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_remux_pipeline_backpressure_tests
    COMMAND mw_remux_pipeline_backpressure_tests)
set_tests_properties(mw_remux_pipeline_backpressure_tests PROPERTIES
    TIMEOUT 60 LABELS "pipeline;backpressure")
