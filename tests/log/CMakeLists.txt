add_executable(mw_log_tests logging_test.cc logging_c_probe.c)
target_link_libraries(mw_log_tests PRIVATE Catch2::Catch2WithMain mw::log)
add_test(NAME mw_log_tests COMMAND mw_log_tests)
