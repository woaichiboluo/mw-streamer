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

add_executable(mw_input_player_proxy_tests
    input_player_proxy_test.cc
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

add_executable(mw_packet_queue_tests
    packet_queue_test.cc
)

target_compile_definitions(mw_packet_queue_tests
    PRIVATE
        MW_PACKET_QUEUE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_packet_queue_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer_test
)

add_test(NAME mw_packet_queue_tests COMMAND mw_packet_queue_tests)

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

add_executable(mw_output_session_tests
    output_session_test.cc
)

target_compile_definitions(mw_output_session_tests
    PRIVATE
        MW_OUTPUT_SESSION_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_output_session_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_output_session_tests COMMAND mw_output_session_tests)
