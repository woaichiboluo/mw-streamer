# Test stage latency and throughput on the current pipeline.
add_executable(mw_latency_unit_tests
    latency_test.cc
    "${PROJECT_SOURCE_DIR}/src/streamer/performance/latency.cc"
    "${PROJECT_SOURCE_DIR}/src/streamer/performance/settings.cc")
target_include_directories(mw_latency_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mw_latency_unit_tests PRIVATE
    Catch2::Catch2WithMain mw::log FFmpeg::avutil)
mw_copy_test_runtime_dlls(mw_latency_unit_tests)
add_test(NAME mw_latency_unit_tests COMMAND mw_latency_unit_tests)
set_tests_properties(mw_latency_unit_tests PROPERTIES TIMEOUT 30)

add_executable(mw_stage_latency_tests
    input_decoder_latency_test.cc
    encoder_latency_test.cc
    media_filter_latency_test.cc
    converter_latency_test.cc
    syncer_latency_test.cc
    remuxer_latency_test.cc)
target_compile_definitions(mw_stage_latency_tests PRIVATE
    MW_STREAMER_LATENCY_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
    MW_STREAMER_PERFORMANCE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_stage_latency_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
mw_copy_test_runtime_dlls(mw_stage_latency_tests)
add_test(NAME mw_stage_latency_tests COMMAND mw_stage_latency_tests)
set_tests_properties(mw_stage_latency_tests PROPERTIES TIMEOUT 90)
