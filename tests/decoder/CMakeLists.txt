add_executable(mw_av_decoder_tests
    av_decoder_test.cc av_decoder_pipeline_test.cc av_decoder_hardware_test.cc
)
target_compile_definitions(mw_av_decoder_tests PRIVATE
    MW_AV_DECODER_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_decoder_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain fmt::fmt
)
add_test(NAME mw_av_decoder_tests COMMAND mw_av_decoder_tests)
set_tests_properties(mw_av_decoder_tests PROPERTIES TIMEOUT 60)
mw_copy_test_runtime_dlls(mw_av_decoder_tests)
