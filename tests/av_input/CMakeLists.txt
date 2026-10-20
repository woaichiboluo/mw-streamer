add_executable(mw_av_input_tests
    av_input_test.cc av_dictionary_test.cc av_input_proxy_test.cc
)
target_compile_definitions(mw_av_input_tests PRIVATE
    MW_AV_INPUT_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_input_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::avformat fmt::fmt
)
add_test(NAME mw_av_input_tests COMMAND mw_av_input_tests)
set_tests_properties(mw_av_input_tests PROPERTIES TIMEOUT 60)

add_executable(mw_av_probe_tests av_probe_test.cc)
target_compile_definitions(mw_av_probe_tests PRIVATE
    MW_AV_INPUT_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_probe_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::avformat fmt::fmt
)
add_test(NAME mw_av_probe_tests COMMAND mw_av_probe_tests)
set_tests_properties(mw_av_probe_tests PROPERTIES TIMEOUT 60)
