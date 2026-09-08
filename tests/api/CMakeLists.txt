add_executable(mw_processor_c_api_tests
    processor_c_api_test.c
)

target_include_directories(mw_processor_c_api_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

add_test(NAME mw_processor_c_api_tests COMMAND mw_processor_c_api_tests)
