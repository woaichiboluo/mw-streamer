add_executable(mw_streamer_init_tests init_test.cc srt_lifecycle_test.cc)
target_compile_definitions(mw_streamer_init_tests PRIVATE
    MW_STREAMER_INIT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
    "$<$<PLATFORM_ID:Windows>:NOMINMAX>"
)
target_link_libraries(mw_streamer_init_tests PRIVATE Catch2::Catch2WithMain mw::streamer mw::log SRT::SRT)
if(NOT BUILD_SHARED)
    target_link_libraries(mw_streamer_init_tests PRIVATE ZLMediaKit::MediaKit)
endif()
add_test(NAME mw_streamer_init_tests COMMAND mw_streamer_init_tests)

if(BUILD_SHARED)
    # Exercise SDK ownership through the real API without crossing SDK copies.
    add_executable(mw_streamer_lifecycle_tests init_test.cc)
    target_link_libraries(mw_streamer_lifecycle_tests PRIVATE
        Catch2::Catch2WithMain mw_streamer_test_static ZLMediaKit::MediaKit mw::log)
    add_test(NAME mw_streamer_lifecycle_tests COMMAND mw_streamer_lifecycle_tests)
endif()
