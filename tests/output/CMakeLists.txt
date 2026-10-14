add_executable(mw_output_tests output_test.cc)
target_link_libraries(mw_output_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain)
add_test(NAME mw_output_tests COMMAND mw_output_tests)
set_tests_properties(mw_output_tests PROPERTIES TIMEOUT 60)

add_executable(mw_encode_push_sink_tests encode_push_sink_test.cc)
target_link_libraries(mw_encode_push_sink_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::avformat)
if(WIN32)
    target_compile_definitions(mw_encode_push_sink_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(mw_encode_push_sink_tests PRIVATE ws2_32)
endif()
add_test(NAME mw_encode_push_sink_tests COMMAND mw_encode_push_sink_tests)
set_tests_properties(mw_encode_push_sink_tests PROPERTIES
    TIMEOUT 60 LABELS "encode_push_sink")
mw_copy_test_runtime_dlls(mw_encode_push_sink_tests)
