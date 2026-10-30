add_executable(mw_scheduler_tests scheduler_test.cc realtime_timing_test.cc)
target_include_directories(mw_scheduler_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(mw_scheduler_tests PRIVATE
    MW_SCHEDULER_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_scheduler_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain
)
add_test(NAME mw_scheduler_tests COMMAND mw_scheduler_tests)
set_tests_properties(mw_scheduler_tests PROPERTIES TIMEOUT 90)
mw_copy_test_runtime_dlls(mw_scheduler_tests)
