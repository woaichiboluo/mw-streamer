add_executable(mw_platform_tests
    thread_test.cc blocking_queue_test.cc wait_signal_test.cc)
target_link_libraries(mw_platform_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain Threads::Threads)
add_test(NAME mw_platform_tests COMMAND mw_platform_tests)
set_tests_properties(mw_platform_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_platform_tests)
