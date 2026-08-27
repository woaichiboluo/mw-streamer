add_executable(mw_streaming_standby_tests
    streaming_standby_test.cc
)

target_link_libraries(mw_streaming_standby_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_streaming_standby_tests COMMAND mw_streaming_standby_tests)

add_executable(mw_streaming_pipeline_tests
    streaming_pipeline_test.cc
)

target_compile_definitions(mw_streaming_pipeline_tests
    PRIVATE
        MW_STREAMING_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_streaming_pipeline_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        mw::streamer_test
)

add_test(NAME mw_streaming_pipeline_tests COMMAND mw_streaming_pipeline_tests)

add_executable(mw_file_pipeline_tests
    file_pipeline_test.cc
)

target_compile_definitions(mw_file_pipeline_tests
    PRIVATE
        MW_FILE_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_file_pipeline_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_file_pipeline_tests COMMAND mw_file_pipeline_tests)

add_executable(mw_remux_pipeline_tests
    remux_pipeline_test.cc
)

target_compile_definitions(mw_remux_pipeline_tests
    PRIVATE
        MW_REMUX_PIPELINE_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_remux_pipeline_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::MediaKit
        mw::streamer_test
)

add_test(NAME mw_remux_pipeline_tests COMMAND mw_remux_pipeline_tests)

add_executable(mw_processing_chain_tests
    processing_chain_test.cc
)

target_compile_definitions(mw_processing_chain_tests
    PRIVATE
        MW_PROCESSING_CHAIN_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_processing_chain_tests
    PRIVATE
        Catch2::Catch2WithMain
        HdrHistogram::HdrHistogram
        ZLMediaKit::MediaKit
        mw::streamer_test
)

add_test(NAME mw_processing_chain_tests COMMAND mw_processing_chain_tests)

add_executable(mw_source_output_worker_tests
    source_output_worker_test.cc
)

target_link_libraries(mw_source_output_worker_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avcodec
        mw::streamer_test
)

add_test(NAME mw_source_output_worker_tests COMMAND mw_source_output_worker_tests)

add_executable(mw_output_event_mailbox_tests
    output_event_mailbox_test.cc
)

target_link_libraries(mw_output_event_mailbox_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_output_event_mailbox_tests COMMAND mw_output_event_mailbox_tests)

add_executable(mw_output_sink_worker_tests
    output_sink_worker_test.cc
)

target_link_libraries(mw_output_sink_worker_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        mw::streamer_test
)

add_test(NAME mw_output_sink_worker_tests COMMAND mw_output_sink_worker_tests)
