add_executable(mw_force_fps_tests force_fps_test.cc)
target_link_libraries(mw_force_fps_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain
)
add_test(NAME mw_force_fps_tests COMMAND mw_force_fps_tests)
set_tests_properties(mw_force_fps_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_force_fps_tests)
