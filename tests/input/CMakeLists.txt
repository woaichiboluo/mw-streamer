add_executable(mw_zlm_input_tests zlm_input_test.cc zlm_input_test_main.cc)
target_compile_definitions(mw_zlm_input_tests PRIVATE
    MW_STREAMER_INPUT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)
target_link_libraries(mw_zlm_input_tests PRIVATE Catch2::Catch2 mw::streamer)
if(BUILD_SHARED)
    target_compile_definitions(mw_zlm_input_tests PRIVATE MW_STREAMER_INPUT_TEST_SHARED)
endif()
add_test(NAME mw_zlm_input_tests COMMAND mw_zlm_input_tests)

# Public endpoints are exercised manually, outside the deterministic test suite.
add_executable(mw_zlm_input_probe zlm_input_probe.cc)
target_link_libraries(mw_zlm_input_probe PRIVATE mw::streamer)
