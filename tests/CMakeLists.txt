add_executable(
  mw_streamer_tests
  aac_encoder_test.cpp
  aligned_audio_buffer_test.cpp
  audio_converter_test.cpp
  audio_fifo_test.cpp
  blocking_queue_test.cpp
  compressed_packet_delay_buffer_test.cpp
  error_test.cpp
  ffmpeg_log_test.cpp
  ffmpeg_interrupt_test.cpp
  ffmpeg_raii_test.cpp
  fragmented_mp4_muxer_test.cpp
  input_mode_test.cpp
  input_options_test.cpp
  network_muxer_test.cpp
  output_worker_test.cpp
  pipeline_config_test.cpp
  pipeline_test.cpp
  processor_context_test.cpp
  processor_test.cpp
  reconnect_backoff_test.cpp
  status_test.cpp
  timestamp_test.cpp
  video_synchronizer_test.cpp
  version_test.cpp
)
target_link_libraries(
  mw_streamer_tests
  PRIVATE
    mw::streamer
    FFmpeg::FFmpeg
    GTest::gtest_main
    GTest::gmock
    spdlog::spdlog
)

include(GoogleTest)
gtest_discover_tests(mw_streamer_tests DISCOVERY_MODE PRE_TEST)

add_executable(
  mw_streamer_processor_c_header_test
  processor_c_header_test.c
)
target_include_directories(
  mw_streamer_processor_c_header_test
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
add_test(
  NAME ProcessorCHeaderTest.CompilesAsC
  COMMAND mw_streamer_processor_c_header_test
)

add_executable(
  mw_streamer_file_integration_tests
  audio_auto_decoder_integration_test.cpp
  demuxer_integration_test.cpp
  fragmented_mp4_muxer_integration_test.cpp
  network_muxer_integration_test.cpp
  output_worker_integration_test.cpp
  standby_image_cpu_integration_test.cpp
)
target_compile_definitions(
  mw_streamer_file_integration_tests
  PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
target_link_libraries(
  mw_streamer_file_integration_tests
  PRIVATE
    mw::streamer
    FFmpeg::FFmpeg
    GTest::gtest_main
)
gtest_discover_tests(
  mw_streamer_file_integration_tests
  DISCOVERY_MODE PRE_TEST
  NO_PRETTY_VALUES
  PROPERTIES
    LABELS "file-integration"
    TIMEOUT 30
)

add_executable(
  mw_streamer_gpu_integration_tests
  ffmpeg_cuda_smoke_test.cpp
  nv_decoder_integration_test.cpp
  nv_encoder_integration_test.cpp
  pipeline_file_e2e_test.cpp
  standby_image_gpu_integration_test.cpp
)
target_compile_definitions(
  mw_streamer_gpu_integration_tests
  PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
target_link_libraries(
  mw_streamer_gpu_integration_tests
  PRIVATE
    mw::streamer
    FFmpeg::FFmpeg
    GTest::gtest_main
)
gtest_discover_tests(
  mw_streamer_gpu_integration_tests
  DISCOVERY_MODE PRE_TEST
  NO_PRETTY_VALUES
  PROPERTIES
    LABELS "gpu-integration"
    RUN_SERIAL TRUE
    TIMEOUT 30
)
