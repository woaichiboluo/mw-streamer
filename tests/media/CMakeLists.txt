add_executable(mw_audio_encoder_tests
    audio_encoder_test.cc
)

target_link_libraries(mw_audio_encoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_audio_encoder_tests COMMAND mw_audio_encoder_tests)

add_executable(mw_video_encoder_tests
    video_encoder_test.cc
)

target_link_libraries(mw_video_encoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_video_encoder_tests COMMAND mw_video_encoder_tests)

add_executable(mw_zlm_packet_converter_tests
    av_packet_to_zlm_frame_converter_test.cc
    zlm_packet_converter_test.cc
)

target_compile_definitions(mw_zlm_packet_converter_tests
    PRIVATE
        MW_ZLM_PACKET_CONVERTER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_zlm_packet_converter_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_zlm_packet_converter_tests COMMAND mw_zlm_packet_converter_tests)

add_executable(mw_audio_decoder_tests
    audio_decoder_test.cc
)

target_compile_definitions(mw_audio_decoder_tests
    PRIVATE
        MW_AUDIO_DECODER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_audio_decoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_audio_decoder_tests COMMAND mw_audio_decoder_tests)

add_executable(mw_audio_resampler_tests
    audio_resampler_test.cc
)

target_link_libraries(mw_audio_resampler_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_audio_resampler_tests COMMAND mw_audio_resampler_tests)

add_executable(mw_video_decoder_tests
    video_decoder_test.cc
)

target_compile_definitions(mw_video_decoder_tests
    PRIVATE
        MW_VIDEO_DECODER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_video_decoder_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_video_decoder_tests COMMAND mw_video_decoder_tests)

add_executable(mw_ffmpeg_types_tests
    ffmpeg_types_test.cc
)

target_compile_definitions(mw_ffmpeg_types_tests
    PRIVATE
        MW_FFMPEG_TYPES_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_ffmpeg_types_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_ffmpeg_types_tests COMMAND mw_ffmpeg_types_tests)

add_executable(mw_hardware_context_tests
    hardware_context_test.cc
)

target_link_libraries(mw_hardware_context_tests
    PRIVATE
        Catch2::Catch2WithMain
        CUDA::cuda_driver
        mw::streamer_test
)

add_test(NAME mw_hardware_context_tests COMMAND mw_hardware_context_tests)
