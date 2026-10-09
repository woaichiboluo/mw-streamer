add_executable(mw_async_remuxer_media_timestamp_converter_tests
    async_remuxer_media_timestamp_converter_test.cc)
target_link_libraries(mw_async_remuxer_media_timestamp_converter_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_async_remuxer_media_timestamp_converter_tests
    COMMAND mw_async_remuxer_media_timestamp_converter_tests)
set_tests_properties(mw_async_remuxer_media_timestamp_converter_tests PROPERTIES TIMEOUT 60)

add_executable(mw_async_remuxer_packet_interleaver_tests
    async_remuxer_packet_interleaver_test.cc)
target_link_libraries(mw_async_remuxer_packet_interleaver_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer)
add_test(NAME mw_async_remuxer_packet_interleaver_tests
    COMMAND mw_async_remuxer_packet_interleaver_tests)
set_tests_properties(mw_async_remuxer_packet_interleaver_tests PROPERTIES TIMEOUT 60)

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
    TIMEOUT 60 LABELS "remuxer_shared")

add_executable(mw_async_remuxer_recording_tests async_remuxer_recording_test.cc)
target_link_libraries(mw_async_remuxer_recording_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat)
add_test(NAME mw_async_remuxer_recording_tests COMMAND mw_async_remuxer_recording_tests)
set_tests_properties(mw_async_remuxer_recording_tests PROPERTIES TIMEOUT 60)

add_executable(mw_async_remuxer_network_tests async_remuxer_network_test.cc)
target_link_libraries(mw_async_remuxer_network_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat SRT::SRT
    ZLMediaKit::MediaKit ZLMediaKit::ext-codec)
if(BUILD_SHARED)
    target_link_libraries(mw_async_remuxer_network_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_async_remuxer_network_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_async_remuxer_network_tests COMMAND mw_async_remuxer_network_tests)
set_tests_properties(mw_async_remuxer_network_tests PROPERTIES TIMEOUT 60)

add_executable(mw_async_remuxer_publish_probe async_remuxer_publish_probe.cc)
target_link_libraries(mw_async_remuxer_publish_probe PRIVATE mw::streamer fmt::fmt)

add_executable(mw_sync_remuxer_tests sync_remuxer_test.cc)
target_compile_definitions(mw_sync_remuxer_tests PRIVATE
    MW_STREAMER_SYNC_REMUXER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_sync_remuxer_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat ZLMediaKit::MediaKit)
if(BUILD_SHARED)
    target_link_libraries(mw_sync_remuxer_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_sync_remuxer_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_sync_remuxer_tests COMMAND mw_sync_remuxer_tests)
set_tests_properties(mw_sync_remuxer_tests PROPERTIES TIMEOUT 60)

add_executable(mw_sync_remuxer_network_tests sync_remuxer_network_test.cc)
target_compile_definitions(mw_sync_remuxer_network_tests PRIVATE
    MW_STREAMER_SYNC_REMUXER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
target_link_libraries(mw_sync_remuxer_network_tests PRIVATE
    Catch2::Catch2WithMain FFmpeg::avformat ZLMediaKit::MediaKit)
if(BUILD_SHARED)
    target_link_libraries(mw_sync_remuxer_network_tests PRIVATE mw_streamer_test_static)
else()
    target_link_libraries(mw_sync_remuxer_network_tests PRIVATE mw::streamer)
endif()
add_test(NAME mw_sync_remuxer_network_tests COMMAND mw_sync_remuxer_network_tests)
set_tests_properties(mw_sync_remuxer_network_tests PROPERTIES TIMEOUT 30)

set_tests_properties(
    mw_async_remuxer_media_timestamp_converter_tests
    mw_async_remuxer_packet_interleaver_tests
    mw_async_remuxer_recording_tests
    mw_async_remuxer_network_tests
    PROPERTIES LABELS "async_remuxer")
set_tests_properties(mw_sync_remuxer_tests mw_sync_remuxer_network_tests
    PROPERTIES LABELS "sync_remuxer")

foreach(kind async sync)
    set(hls_test_target mw_${kind}_remuxer_hls_tests)
    add_executable(${hls_test_target} ${kind}_remuxer_hls_test.cc)
    target_link_libraries(${hls_test_target} PRIVATE
        Catch2::Catch2WithMain FFmpeg::avformat ZLMediaKit::MediaKit)
    if(BUILD_SHARED)
        target_link_libraries(${hls_test_target} PRIVATE mw_streamer_test_static)
    else()
        target_link_libraries(${hls_test_target} PRIVATE mw::streamer)
    endif()
    add_test(NAME ${hls_test_target} COMMAND ${hls_test_target})
    set_tests_properties(${hls_test_target} PROPERTIES
        TIMEOUT 60 LABELS "${kind}_remuxer;hls")
endforeach()
