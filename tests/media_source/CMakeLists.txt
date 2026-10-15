add_executable(mw_av_media_source_tests av_media_source_test.cc)
target_compile_definitions(mw_av_media_source_tests PRIVATE
    MW_AV_MEDIA_SOURCE_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_media_source_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain fmt::fmt
)
add_test(NAME mw_av_media_source_tests COMMAND mw_av_media_source_tests)
set_tests_properties(mw_av_media_source_tests PROPERTIES TIMEOUT 60)
mw_copy_test_runtime_dlls(mw_av_media_source_tests)
