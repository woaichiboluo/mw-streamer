add_executable(mw_pipeline_tests pipeline_test.cc)

target_compile_definitions(mw_pipeline_tests
    PRIVATE
        MW_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_pipeline_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        mw::streamer_test
)

add_test(NAME mw_pipeline_tests COMMAND mw_pipeline_tests)

add_executable(mw_pipeline_fatal_tests pipeline_fatal_test.cc)

target_compile_definitions(mw_pipeline_fatal_tests
    PRIVATE
        MW_PIPELINE_FATAL_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_pipeline_fatal_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        mw::streamer_test
)

add_test(NAME mw_pipeline_fatal_tests COMMAND mw_pipeline_fatal_tests)

add_executable(mw_remux_sink_tests
    remux_sink_test.cc
    remux_sink_lifecycle_test.cc
    remux_startup_cache_test.cc
)

target_compile_definitions(mw_remux_sink_tests
    PRIVATE MW_REMUX_SINK_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_remux_sink_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avformat
        FFmpeg::avcodec
        ZLMediaKit::MediaKit
        ZLMediaKit::ext-codec
        mw::streamer_test
)

add_test(NAME mw_remux_sink_tests COMMAND mw_remux_sink_tests)

add_executable(mw_pipeline_packet_queue_tests packet_queue_test.cc)

target_link_libraries(mw_pipeline_packet_queue_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_pipeline_packet_queue_tests COMMAND mw_pipeline_packet_queue_tests)

add_executable(mw_decoder_sink_tests
    decoder_sink_test.cc
    offline_pipeline_test.cc
)

target_compile_definitions(mw_decoder_sink_tests
    PRIVATE
        MW_DECODER_SINK_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_decoder_sink_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avformat
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_decoder_sink_tests COMMAND mw_decoder_sink_tests)

add_executable(mw_decoder_sink_cuda_tests decoder_sink_cuda_test.cc)

target_compile_definitions(mw_decoder_sink_cuda_tests
    PRIVATE
        MW_DECODER_SINK_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_decoder_sink_cuda_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avformat
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_decoder_sink_cuda_tests COMMAND mw_decoder_sink_cuda_tests)

add_executable(mw_encoder_sink_tests encoder_sink_test.cc)
target_link_libraries(mw_encoder_sink_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avcodec FFmpeg::avutil mw::streamer_test)
add_test(NAME mw_encoder_sink_tests COMMAND mw_encoder_sink_tests)

add_executable(mw_encoder_sink_cuda_tests encoder_sink_cuda_test.cc)
target_link_libraries(mw_encoder_sink_cuda_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avcodec FFmpeg::avutil mw::streamer_test)
add_test(NAME mw_encoder_sink_cuda_tests COMMAND mw_encoder_sink_cuda_tests)

add_executable(mw_synchronizer_sink_tests
    synchronizer_sink_test.cc
    realtime_frame_scheduler_test.cc)
target_link_libraries(mw_synchronizer_sink_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avutil mw::streamer_test)
add_test(NAME mw_synchronizer_sink_tests COMMAND mw_synchronizer_sink_tests)
add_test(NAME mw_synchronizer_sink_cuda_tests
    COMMAND mw_synchronizer_sink_tests "[.cuda]")

add_executable(mw_encoder_pipeline_tests encoder_pipeline_test.cc)
target_compile_definitions(mw_encoder_pipeline_tests PRIVATE
    MW_ENCODER_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_encoder_pipeline_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
    mw::streamer_test)
add_test(NAME mw_encoder_pipeline_tests COMMAND mw_encoder_pipeline_tests)

add_executable(mw_pipeline_builder_tests pipeline_builder_test.cc)
target_compile_definitions(mw_pipeline_builder_tests PRIVATE
    MW_PIPELINE_BUILDER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_pipeline_builder_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
    mw::streamer_test)
add_test(NAME mw_pipeline_builder_tests COMMAND mw_pipeline_builder_tests)

add_executable(mw_processor_sink_tests processor_sink_test.cc)

target_link_libraries(mw_processor_sink_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_processor_sink_tests COMMAND mw_processor_sink_tests)

add_executable(mw_sink_message_tests sink_message_test.cc)
target_sources(mw_sink_message_tests PRIVATE sink_test.cc)

target_link_libraries(mw_sink_message_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_sink_message_tests COMMAND mw_sink_message_tests)
