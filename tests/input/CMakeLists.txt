add_executable(mw_ffmpeg_input_tests
    ffmpeg_input_test.cc
    ffmpeg_input_packet_test.cc
    ffmpeg_input_options_test.cc
    ffmpeg_input_decode_test.cc
    ffmpeg_input_source_test.cc
    ffmpeg_input_selection_test.cc
    ffmpeg_packet_reader_test.cc
    ffmpeg_frame_decoder_test.cc
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

if(BUILD_SHARED)
    # Exercise native decoding and input options through the DLL's public API.
    add_executable(mw_ffmpeg_input_public_tests
        ffmpeg_input_decode_test.cc ffmpeg_input_source_test.cc ffmpeg_input_selection_test.cc ffmpeg_input_options_test.cc input_test_main.cc)
    target_compile_definitions(mw_ffmpeg_input_public_tests PRIVATE
        MW_STREAMER_INPUT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
    target_link_libraries(mw_ffmpeg_input_public_tests PRIVATE
        Catch2::Catch2 FFmpeg::avformat mw::streamer fmt::fmt)
    add_test(NAME mw_ffmpeg_input_public_tests COMMAND mw_ffmpeg_input_public_tests)
    set_tests_properties(mw_ffmpeg_input_public_tests PROPERTIES TIMEOUT 45)
endif()

# Public endpoints are exercised manually, outside the deterministic test suite.
add_executable(mw_ffmpeg_input_probe ffmpeg_input_probe.cc)
target_link_libraries(mw_ffmpeg_input_probe PRIVATE mw::streamer fmt::fmt)
