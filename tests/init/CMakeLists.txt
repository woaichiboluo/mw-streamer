add_executable(mw_streamer_init_tests init_test.cc srt_lifecycle_test.cc)
target_compile_definitions(mw_streamer_init_tests PRIVATE
    MW_STREAMER_INIT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)
target_link_libraries(mw_streamer_init_tests PRIVATE Catch2::Catch2WithMain mw::streamer mw::log SRT::SRT)
add_test(NAME mw_streamer_init_tests COMMAND mw_streamer_init_tests)

if(WIN32 AND BUILD_SHARED)
    add_library(mw_streamer_unload_fixture SHARED unload_fixture.cc)
    target_link_libraries(mw_streamer_unload_fixture PRIVATE mw::streamer mw::log)
    add_executable(mw_streamer_unload_host unload_host.cc)
    add_dependencies(mw_streamer_unload_host mw_streamer_unload_fixture)
    add_test(NAME mw_streamer_unload_tests
        COMMAND mw_streamer_unload_host
            "$<TARGET_FILE:mw_streamer_unload_fixture>"
            "${MW_STREAMER_TEST_DATA_DIR}/h264_aac.mp4"
            "${CMAKE_CURRENT_BINARY_DIR}/unload.log"
    )
    set_tests_properties(mw_streamer_unload_tests PROPERTIES TIMEOUT 45)
endif()
