add_executable(mw_processor_c_api_tests
    processor_c_api_test.c
)

target_include_directories(mw_processor_c_api_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

add_test(NAME mw_processor_c_api_tests COMMAND mw_processor_c_api_tests)

add_executable(mw_c_api_tests
    c_api_test.c
)

target_link_libraries(mw_c_api_tests
    PRIVATE
        mw::streamer
)

# The public API is C, while the implementation resides in the C++ library.
set_target_properties(mw_c_api_tests PROPERTIES LINKER_LANGUAGE CXX)

add_test(NAME mw_c_api_tests COMMAND mw_c_api_tests)
