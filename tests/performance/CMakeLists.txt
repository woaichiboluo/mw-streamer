add_executable(mw_performance_tests performance_test.cc)
target_compile_definitions(mw_performance_tests PRIVATE
    MW_STREAMER_PERFORMANCE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_performance_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer mw::log)
if(BUILD_SHARED)
    # Internal counters are not exported; keep the public pipeline tests on the DLL.
    target_sources(mw_performance_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/streamer/performance/performance.cc")
endif()
add_test(NAME mw_performance_tests COMMAND mw_performance_tests)
set_tests_properties(mw_performance_tests PROPERTIES TIMEOUT 30)

add_executable(mw_remuxer_performance_tests
    remuxer_performance_test.cc)
target_link_libraries(mw_remuxer_performance_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer mw::log)
add_test(NAME mw_remuxer_performance_tests
    COMMAND mw_remuxer_performance_tests)
set_tests_properties(mw_remuxer_performance_tests PROPERTIES
    TIMEOUT 45 LABELS "remuxer")
