add_executable(mw_scheduler_tests scheduler_test.cc)
target_compile_definitions(mw_scheduler_tests PRIVATE
    MW_SCHEDULER_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
target_link_libraries(mw_scheduler_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain)
add_test(NAME mw_scheduler_tests COMMAND mw_scheduler_tests)
set_tests_properties(mw_scheduler_tests PROPERTIES TIMEOUT 60)
