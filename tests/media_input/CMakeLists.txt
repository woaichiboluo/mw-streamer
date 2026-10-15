add_executable(mw_av_media_input_tests av_media_input_test.cc)
target_compile_definitions(mw_av_media_input_tests PRIVATE
    MW_AV_MEDIA_INPUT_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
)
target_link_libraries(mw_av_media_input_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain fmt::fmt
)
add_test(NAME mw_av_media_input_tests COMMAND mw_av_media_input_tests)
set_tests_properties(mw_av_media_input_tests PROPERTIES TIMEOUT 60)

add_executable(mw_timestamp_discontinuity_tests
    timestamp_discontinuity_test.cc
    ${PROJECT_SOURCE_DIR}/src/streamer/input/timestamp_discontinuity.cc
)
target_include_directories(mw_timestamp_discontinuity_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(mw_timestamp_discontinuity_tests PRIVATE
    mw::streamer mw::log FFmpeg::avcodec FFmpeg::avutil Catch2::Catch2WithMain
)
add_test(NAME mw_timestamp_discontinuity_tests
    COMMAND mw_timestamp_discontinuity_tests
)
set_tests_properties(mw_timestamp_discontinuity_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_timestamp_discontinuity_tests)
