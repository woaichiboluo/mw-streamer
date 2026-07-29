add_executable(mw_processor_c_api_tests
    processor_c_api_test.c
)

target_include_directories(mw_processor_c_api_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

add_test(
    NAME mw_processor_c_api_tests
    COMMAND mw_processor_c_api_tests
)

add_executable(mw_frame_adapter_tests
    frame_adapter_test.cc
)

target_link_libraries(mw_frame_adapter_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)

add_test(
    NAME mw_frame_adapter_tests
    COMMAND mw_frame_adapter_tests
)

add_executable(mw_processor_handler_tests
    processor_handler_test.cc
)

target_link_libraries(mw_processor_handler_tests
    PRIVATE
        Catch2::Catch2WithMain
        CUDA::cuda_driver
        mw::streamer
)

add_test(
    NAME mw_processor_handler_tests
    COMMAND mw_processor_handler_tests
)

add_executable(mw_decoder_config_tests
    decoder_config_test.cc
)

target_include_directories(mw_decoder_config_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_decoder_config_tests
    PRIVATE
        Catch2::Catch2WithMain
)

add_test(
    NAME mw_decoder_config_tests
    COMMAND mw_decoder_config_tests
)

add_executable(mw_logging_tests
    logging_test.cc
)

target_link_libraries(mw_logging_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer
)

include(Catch)
catch_discover_tests(mw_logging_tests)

add_executable(mw_init_tests
    init_test.cc
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
    srt_init_test.cc
)

target_link_libraries(mw_srt_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::SRT
        mw::streamer
)

catch_discover_tests(mw_srt_init_tests)

add_executable(mw_file_player_tests
    file_player_test.cc
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

add_executable(mw_zlm_packet_converter_tests
    av_packet_to_zlm_frame_converter_test.cc
    zlm_packet_converter_test.cc
)

target_compile_definitions(mw_zlm_packet_converter_tests
    PRIVATE
        MW_ZLM_PACKET_CONVERTER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_zlm_packet_converter_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer
)

add_test(
    NAME mw_zlm_packet_converter_tests
    COMMAND mw_zlm_packet_converter_tests
)

add_executable(mw_audio_decoder_tests
    audio_decoder_test.cc
)

target_compile_definitions(mw_audio_decoder_tests
    PRIVATE
        MW_AUDIO_DECODER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_audio_decoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer
)

add_test(
    NAME mw_audio_decoder_tests
    COMMAND mw_audio_decoder_tests
)

add_executable(mw_audio_resampler_tests
    audio_resampler_test.cc
)

target_link_libraries(mw_audio_resampler_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)

add_test(
    NAME mw_audio_resampler_tests
    COMMAND mw_audio_resampler_tests
)

add_executable(mw_video_decoder_tests
    video_decoder_test.cc
)

target_compile_definitions(mw_video_decoder_tests
    PRIVATE
        MW_VIDEO_DECODER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_video_decoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer
)

add_test(
    NAME mw_video_decoder_tests
    COMMAND mw_video_decoder_tests
)

add_executable(mw_ffmpeg_types_tests
    ffmpeg_types_test.cc
)

target_link_libraries(mw_ffmpeg_types_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
)

add_test(
    NAME mw_ffmpeg_types_tests
    COMMAND mw_ffmpeg_types_tests
)

add_executable(mw_hardware_context_tests
    hardware_context_test.cc
)

target_link_libraries(mw_hardware_context_tests
    PRIVATE
        Catch2::Catch2WithMain
        CUDA::cuda_driver
        mw::streamer
)

add_test(
    NAME mw_hardware_context_tests
    COMMAND mw_hardware_context_tests
)

add_executable(mw_input_player_proxy_tests
    input_player_proxy_test.cc
)

target_compile_definitions(mw_input_player_proxy_tests
    PRIVATE
        MW_INPUT_PLAYER_PROXY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_input_player_proxy_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer
)

add_test(
    NAME mw_input_player_proxy_tests
    COMMAND mw_input_player_proxy_tests
)

add_executable(mw_packet_queue_tests
    packet_queue_test.cc
)

target_compile_definitions(mw_packet_queue_tests
    PRIVATE
        MW_PACKET_QUEUE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_packet_queue_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer
)

add_test(
    NAME mw_packet_queue_tests
    COMMAND mw_packet_queue_tests
)

add_executable(mw_recording_target_tests
    recording_target_test.cc
)

target_compile_definitions(mw_recording_target_tests
    PRIVATE
        MW_RECORDING_TARGET_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_recording_target_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
)

add_test(
    NAME mw_recording_target_tests
    COMMAND mw_recording_target_tests
)

add_executable(mw_output_session_tests
    output_session_test.cc
)

target_compile_definitions(mw_output_session_tests
    PRIVATE
        MW_OUTPUT_SESSION_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(mw_output_session_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer
)

add_test(
    NAME mw_output_session_tests
    COMMAND mw_output_session_tests
)

add_executable(mw_zlm_logger_lifecycle_tests
    zlm_logger_lifecycle_test.cc
)

target_link_libraries(mw_zlm_logger_lifecycle_tests
    PRIVATE
        ZLMediaKit::ToolKit
)

add_test(
    NAME mw_zlm_logger_lifecycle_tests
    COMMAND mw_zlm_logger_lifecycle_tests
)

add_subdirectory(e2e)
