add_executable(mw_av_frame_source_tests
    av_frame_source_test.cc av_frame_source_pipeline_test.cc
)
target_compile_definitions(mw_av_frame_source_tests PRIVATE
    MW_AV_FRAME_SOURCE_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_frame_source_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::avfilter fmt::fmt
)
add_test(NAME mw_av_frame_source_tests COMMAND mw_av_frame_source_tests)
set_tests_properties(mw_av_frame_source_tests PROPERTIES TIMEOUT 60)
