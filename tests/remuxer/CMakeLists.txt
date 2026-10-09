add_executable(mw_media_timestamp_converter_tests
    media_timestamp_converter_test.cc)
target_link_libraries(mw_media_timestamp_converter_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_media_timestamp_converter_tests
    COMMAND mw_media_timestamp_converter_tests)
set_tests_properties(mw_media_timestamp_converter_tests PROPERTIES TIMEOUT 60)

add_executable(mw_packet_interleaver_tests packet_interleaver_test.cc)
target_link_libraries(mw_packet_interleaver_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_packet_interleaver_tests COMMAND mw_packet_interleaver_tests)
set_tests_properties(mw_packet_interleaver_tests PROPERTIES TIMEOUT 60)

add_executable(mw_packet_converter_tests packet_converter_test.cc)
target_link_libraries(mw_packet_converter_tests PRIVATE
    Catch2::Catch2WithMain ZLMediaKit::MediaKit ZLMediaKit::ext-codec)
if(BUILD_SHARED)
    target_link_libraries(mw_packet_converter_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_packet_converter_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_packet_converter_tests COMMAND mw_packet_converter_tests)
set_tests_properties(mw_packet_converter_tests PROPERTIES TIMEOUT 60)

add_executable(mw_remuxer_recording_tests remuxer_recording_test.cc)
target_link_libraries(mw_remuxer_recording_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_remuxer_recording_tests COMMAND mw_remuxer_recording_tests)
set_tests_properties(mw_remuxer_recording_tests PROPERTIES TIMEOUT 60)

add_executable(mw_remuxer_network_tests remuxer_network_test.cc)
target_link_libraries(mw_remuxer_network_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat SRT::SRT
    ZLMediaKit::MediaKit ZLMediaKit::ext-codec)
if(BUILD_SHARED)
    target_link_libraries(mw_remuxer_network_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_remuxer_network_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_remuxer_network_tests COMMAND mw_remuxer_network_tests)
set_tests_properties(mw_remuxer_network_tests PROPERTIES TIMEOUT 60)

add_executable(mw_remuxer_publish_probe remuxer_publish_probe.cc)
target_link_libraries(mw_remuxer_publish_probe PRIVATE mw::streamer fmt::fmt)
