add_executable(mw_logging_tests
    logging_test.cpp
)

target_link_libraries(mw_logging_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::SRT
        ZLMediaKit::ToolKit
        mw::streamer
)

include(Catch)
catch_discover_tests(mw_logging_tests)

add_executable(mw_init_tests
    init_test.cpp
)

target_link_libraries(mw_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer
)

catch_discover_tests(mw_init_tests)

add_executable(mw_srt_init_tests
    srt_init_test.cpp
)

target_link_libraries(mw_srt_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::SRT
        mw::streamer
)

catch_discover_tests(mw_srt_init_tests)

add_executable(mw_file_player_tests
    file_player_test.cpp
)

target_compile_definitions(mw_file_player_tests
    PRIVATE
        MW_FILE_PLAYER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_file_player_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::SRT
        ZLMediaKit::ext-codec
)

add_test(NAME mw_file_player_tests COMMAND mw_file_player_tests)

add_executable(mw_zlm_packet_convter_tests
    zlm_packet_convter_test.cpp
)

target_compile_definitions(mw_zlm_packet_convter_tests
    PRIVATE
        MW_ZLM_PACKET_CONVTER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_zlm_packet_convter_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer
)

add_test(
    NAME mw_zlm_packet_convter_tests
    COMMAND mw_zlm_packet_convter_tests
)

add_executable(mw_zlm_logger_lifecycle_tests
    zlm_logger_lifecycle_test.cpp
)

target_link_libraries(mw_zlm_logger_lifecycle_tests
    PRIVATE
        ZLMediaKit::ToolKit
)

add_test(
    NAME mw_zlm_logger_lifecycle_tests
    COMMAND mw_zlm_logger_lifecycle_tests
)
