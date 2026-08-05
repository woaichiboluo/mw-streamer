add_executable(mw_processor_c_api_tests
    processor_c_api_test.c
)

target_include_directories(mw_processor_c_api_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

add_test(NAME mw_processor_c_api_tests COMMAND mw_processor_c_api_tests)

add_executable(mw_c_api_header_tests
    c_api_header_test.c
)

target_include_directories(mw_c_api_header_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_c_api_header_tests
    PRIVATE mw::streamer
)

add_test(NAME mw_c_api_header_tests COMMAND mw_c_api_header_tests)

add_executable(mw_c_api_tests
    c_api_test.cc
)

target_compile_definitions(mw_c_api_tests
    PRIVATE
        MW_C_API_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_c_api_tests
    PRIVATE
        Catch2::Catch2WithMain
        fmt::fmt
        mw::streamer
)

add_test(NAME mw_c_api_tests COMMAND mw_c_api_tests)

if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
        NAME mw_shared_library_abi_tests
        COMMAND ${CMAKE_COMMAND}
            -DLIBRARY=$<TARGET_FILE:mw_streamer>
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_LIST_DIR}/shared_library_abi_test.cmake
    )
endif()
