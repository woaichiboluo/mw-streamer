add_executable(mw_frame_adapter_tests
    frame_adapter_test.cc
)

target_link_libraries(mw_frame_adapter_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_frame_adapter_tests COMMAND mw_frame_adapter_tests)

add_executable(mw_source_info_adapter_tests
    source_info_adapter_test.cc
)

target_link_libraries(mw_source_info_adapter_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_source_info_adapter_tests COMMAND mw_source_info_adapter_tests)

add_executable(mw_processor_enum_converter_tests
    processor_enum_converter_test.cc
    "${PROJECT_SOURCE_DIR}/src/processor/internal/enum_converter.cc"
)

target_include_directories(mw_processor_enum_converter_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_processor_enum_converter_tests
    PRIVATE Catch2::Catch2WithMain
)

add_test(
    NAME mw_processor_enum_converter_tests
    COMMAND mw_processor_enum_converter_tests
)

add_executable(mw_processor_handler_tests
    processor_handler_test.cc
)

target_link_libraries(mw_processor_handler_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_processor_handler_tests COMMAND mw_processor_handler_tests)
