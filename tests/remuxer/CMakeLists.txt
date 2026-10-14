add_executable(mw_remuxer_media_timestamp_converter_tests
    remuxer_media_timestamp_converter_test.cc)
target_link_libraries(mw_remuxer_media_timestamp_converter_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_remuxer_media_timestamp_converter_tests
    COMMAND mw_remuxer_media_timestamp_converter_tests)
set_tests_properties(mw_remuxer_media_timestamp_converter_tests PROPERTIES
    TIMEOUT 60 LABELS "remuxer")
mw_copy_test_runtime_dlls(mw_remuxer_media_timestamp_converter_tests)

add_executable(mw_remuxer_packet_interleaver_tests
    remuxer_packet_interleaver_test.cc)
target_link_libraries(mw_remuxer_packet_interleaver_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_remuxer_packet_interleaver_tests
    COMMAND mw_remuxer_packet_interleaver_tests)
set_tests_properties(mw_remuxer_packet_interleaver_tests PROPERTIES
    TIMEOUT 60 LABELS "remuxer")
mw_copy_test_runtime_dlls(mw_remuxer_packet_interleaver_tests)

add_executable(mw_packet_converter_tests packet_converter_test.cc)
target_link_libraries(mw_packet_converter_tests PRIVATE
    Catch2::Catch2WithMain ZLMediaKit::MediaKit ZLMediaKit::ext-codec)
if(BUILD_SHARED)
    target_link_libraries(mw_packet_converter_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_packet_converter_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_packet_converter_tests COMMAND mw_packet_converter_tests)
set_tests_properties(mw_packet_converter_tests PROPERTIES
    TIMEOUT 60 LABELS "remuxer")
mw_copy_test_runtime_dlls(mw_packet_converter_tests)

# Public lifecycle and media checks use the actual DLL in shared builds.
add_executable(mw_remuxer_tests remuxer_test.cc)
target_compile_definitions(mw_remuxer_tests PRIVATE
    MW_STREAMER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_remuxer_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
if(WIN32)
    target_compile_definitions(mw_remuxer_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(mw_remuxer_tests PRIVATE ws2_32)
endif()
add_test(NAME mw_remuxer_tests COMMAND mw_remuxer_tests)
set_tests_properties(mw_remuxer_tests PROPERTIES
    TIMEOUT 60 LABELS "remuxer")
mw_copy_test_runtime_dlls(mw_remuxer_tests)
