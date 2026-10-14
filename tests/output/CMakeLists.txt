add_executable(mw_output_tests output_test.cc)
target_link_libraries(mw_output_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain)
add_test(NAME mw_output_tests COMMAND mw_output_tests)
set_tests_properties(mw_output_tests PROPERTIES TIMEOUT 60)
