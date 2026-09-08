add_executable(mw_file_player_tests
    file_player_test.cc
)

target_compile_definitions(mw_file_player_tests
    PRIVATE
        MW_FILE_PLAYER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_file_player_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::SRT
        ZLMediaKit::ext-codec
)

add_test(NAME mw_file_player_tests COMMAND mw_file_player_tests)

add_executable(mw_srt_demuxer_tests srt_demuxer_test.cc)

target_link_libraries(mw_srt_demuxer_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
)

add_test(NAME mw_srt_demuxer_tests COMMAND mw_srt_demuxer_tests)

add_executable(mw_input_player_proxy_tests
    input_player_proxy_test.cc
    packet_sink_input_test.cc
    zlm_timestamp_reviser_test.cc
)

target_compile_definitions(mw_input_player_proxy_tests
    PRIVATE
        MW_INPUT_PLAYER_PROXY_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_input_player_proxy_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer_test
)

add_test(NAME mw_input_player_proxy_tests COMMAND mw_input_player_proxy_tests)

add_executable(mw_pipeline_input_tests pipeline_input_test.cc)

target_compile_definitions(mw_pipeline_input_tests
    PRIVATE
        MW_PIPELINE_INPUT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_pipeline_input_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        mw::streamer_test
)

add_test(NAME mw_pipeline_input_tests COMMAND mw_pipeline_input_tests)

add_executable(mw_recording_target_tests
    recording_target_test.cc
)

target_compile_definitions(mw_recording_target_tests
    PRIVATE
        MW_RECORDING_TARGET_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_recording_target_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_recording_target_tests COMMAND mw_recording_target_tests)

add_executable(mw_mov_fragment_seek_tests mov_fragment_seek_test.cc)
target_include_directories(mw_mov_fragment_seek_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/ZLMediaKit/3rdpart/media-server/libmov/source)
target_link_libraries(mw_mov_fragment_seek_tests PRIVATE
    Catch2::Catch2WithMain MediaServer::mov ZLMediaKit::ToolKit)
add_test(NAME mw_mov_fragment_seek_tests COMMAND mw_mov_fragment_seek_tests)

add_executable(mw_file_input_tests file_input_test.cc)

target_compile_definitions(mw_file_input_tests PRIVATE
    MW_FILE_INPUT_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")

target_link_libraries(mw_file_input_tests PRIVATE
    Catch2::Catch2WithMain
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    mw::streamer_test)

add_test(NAME mw_file_input_tests COMMAND mw_file_input_tests)
