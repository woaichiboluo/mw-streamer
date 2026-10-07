add_executable(mw_processor_tests processor_test.cc processor_test_main.cc)
target_link_libraries(mw_processor_tests PRIVATE
    Catch2::Catch2 mw::streamer FFmpeg::avutil fmt::fmt)
add_test(NAME mw_processor_tests COMMAND mw_processor_tests)
set_tests_properties(mw_processor_tests PROPERTIES TIMEOUT 60)

# Manual network content-sync measurement against the generated flash/tone source.
add_executable(mw_processor_sync_probe processor_sync_probe.cc)
target_link_libraries(mw_processor_sync_probe PRIVATE mw::streamer fmt::fmt)
add_test(NAME mw_processor_sync_tests COMMAND mw_processor_sync_probe
    "${MW_STREAMER_TEST_DATA_DIR}/processor_sync_30fps.mp4" 20 cpu
    "${CMAKE_CURRENT_BINARY_DIR}/sync-events.tsv")
set_tests_properties(mw_processor_sync_tests PROPERTIES TIMEOUT 35)

# Public-stream frame/PCM correlation, without assuming audiovisual test markers.
add_executable(mw_processor_network_probe processor_network_probe.cc)
target_link_libraries(mw_processor_network_probe PRIVATE mw::streamer fmt::fmt)

# Manual recording of scheduled Processor output, with its original A/V timing.
add_executable(mw_processor_record_probe processor_record_probe.cc)
target_link_libraries(mw_processor_record_probe PRIVATE
    mw::streamer FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil fmt::fmt)
