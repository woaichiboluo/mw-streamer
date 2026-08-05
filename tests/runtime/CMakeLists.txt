add_executable(mw_common_tests
    common_test.cc
)

target_include_directories(mw_common_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_common_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::ToolKit
)

add_test(NAME mw_common_tests COMMAND mw_common_tests)

add_executable(mw_performance_tests
    performance_test.cc
)

target_link_libraries(mw_performance_tests
    PRIVATE
        Catch2::Catch2WithMain
        HdrHistogram::HdrHistogram
        mw::streamer_test
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mw_performance_tests
        PRIVATE
            "-Wl,--push-state,--no-as-needed"
            FFmpeg::avutil
            "-Wl,--pop-state"
    )
else()
    target_link_libraries(mw_performance_tests PRIVATE FFmpeg::avutil)
endif()

add_test(NAME mw_performance_tests COMMAND mw_performance_tests)

include(Catch)

add_executable(mw_logging_tests
    logging_test.cc
)

target_link_libraries(mw_logging_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer_test
)

catch_discover_tests(mw_logging_tests)

add_executable(mw_init_tests
    init_test.cc
)

target_link_libraries(mw_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::streamer_test
)

catch_discover_tests(mw_init_tests)

add_executable(mw_srt_init_tests
    srt_init_test.cc
)

target_link_libraries(mw_srt_init_tests
    PRIVATE
        Catch2::Catch2WithMain
        ZLMediaKit::SRT
        mw::streamer_test
)

catch_discover_tests(mw_srt_init_tests)

add_executable(mw_zlm_logger_lifecycle_tests
    zlm_logger_lifecycle_test.cc
)

target_link_libraries(mw_zlm_logger_lifecycle_tests
    PRIVATE ZLMediaKit::ToolKit
)

add_test(
    NAME mw_zlm_logger_lifecycle_tests
    COMMAND mw_zlm_logger_lifecycle_tests
)
