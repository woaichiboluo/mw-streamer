add_executable(mw_syncer_tests syncer_test.cc)
target_include_directories(mw_syncer_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(BUILD_SHARED)
    target_link_libraries(mw_syncer_tests PRIVATE
        mw_streamer_test_static Catch2::Catch2WithMain
    )
else()
    target_link_libraries(mw_syncer_tests PRIVATE
        mw::streamer Catch2::Catch2WithMain
    )
endif()
add_test(NAME mw_syncer_tests COMMAND mw_syncer_tests)
set_tests_properties(mw_syncer_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_syncer_tests)
