add_executable(mw_av_media_filter_tests av_media_filter_test.cc)
target_link_libraries(mw_av_media_filter_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain ${CMAKE_DL_LIBS}
)
target_include_directories(mw_av_media_filter_tests SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/nv-codec-headers/include")
add_test(NAME mw_av_media_filter_tests COMMAND mw_av_media_filter_tests)
set_tests_properties(mw_av_media_filter_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_av_media_filter_tests)
