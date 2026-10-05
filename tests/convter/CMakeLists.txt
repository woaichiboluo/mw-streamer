add_executable(mw_zlm_packet_converter_tests
    zlm_packet_converter_test.cc
    ../input/zlm_input_test_main.cc
)

target_compile_definitions(mw_zlm_packet_converter_tests
    PRIVATE
        MW_ZLM_PACKET_CONVERTER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_zlm_packet_converter_tests
    PRIVATE
        Catch2::Catch2
        mw::streamer
)

if(BUILD_SHARED)
    target_compile_definitions(mw_zlm_packet_converter_tests PRIVATE
        MW_STREAMER_INPUT_TEST_SHARED
    )
    # Exercise internal converters without exposing them from the shared library.
    target_sources(mw_zlm_packet_converter_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/streamer/converter/zlm_codec_parameters_converter.cc"
        "${PROJECT_SOURCE_DIR}/src/streamer/converter/zlm_packet_converter.cc"
    )
    target_link_libraries(mw_zlm_packet_converter_tests PRIVATE ZLMediaKit::ext-codec)
endif()

add_test(NAME mw_zlm_packet_converter_tests COMMAND mw_zlm_packet_converter_tests)
