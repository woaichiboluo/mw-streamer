add_executable(mw_ffmpeg_input_tests
    ffmpeg_input_test.cc
    ffmpeg_input_packet_test.cc
    ffmpeg_input_speed_test.cc
    ffmpeg_rtmp_test.cc
    input_test_main.cc
)
target_compile_definitions(mw_ffmpeg_input_tests PRIVATE
    MW_STREAMER_INPUT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)
target_link_libraries(mw_ffmpeg_input_tests PRIVATE
    Catch2::Catch2 FFmpeg::avformat ZLMediaKit::MediaKit fmt::fmt)
if(BUILD_SHARED)
    target_link_libraries(mw_ffmpeg_input_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_ffmpeg_input_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_ffmpeg_input_tests COMMAND mw_ffmpeg_input_tests)
set_tests_properties(mw_ffmpeg_input_tests PROPERTIES TIMEOUT 90)

add_executable(mw_ffmpeg_input_timing_tests ffmpeg_input_timing_test.cc)
target_include_directories(mw_ffmpeg_input_timing_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(mw_ffmpeg_input_timing_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avutil)
add_test(NAME mw_ffmpeg_input_timing_tests COMMAND mw_ffmpeg_input_timing_tests)

# Public endpoints are exercised manually, outside the deterministic test suite.
add_executable(mw_ffmpeg_input_probe ffmpeg_input_probe.cc)
target_link_libraries(mw_ffmpeg_input_probe PRIVATE mw::streamer fmt::fmt)
