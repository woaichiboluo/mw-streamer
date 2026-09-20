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
    operation_recorder_test.cc
    pipeline_snapshot_test.cc
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

# Link the public library: a static internal test library cannot reproduce
# Windows DLL_PROCESS_DETACH ordering. CTest also verifies process termination.
add_executable(mw_runtime_exit_tests runtime_exit_test.cc)
target_link_libraries(mw_runtime_exit_tests PRIVATE mw::streamer)
target_compile_definitions(mw_runtime_exit_tests PRIVATE
    MW_RUNTIME_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}")
foreach(scenario IN ITEMS create immediate-stop shared recreate network-start)
    add_test(NAME mw_runtime_exit_${scenario}
        COMMAND mw_runtime_exit_tests ${scenario})
    set_tests_properties(mw_runtime_exit_${scenario} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(mw_runtime_lifecycle_tests runtime_lifecycle_test.cc)
target_link_libraries(mw_runtime_lifecycle_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer_test fmt::fmt ZLMediaKit::SRT)
add_test(NAME mw_runtime_lifecycle_tests COMMAND mw_runtime_lifecycle_tests)
set_tests_properties(mw_runtime_lifecycle_tests PROPERTIES TIMEOUT 30)

add_executable(mw_startup_ring_tests startup_ring_test.cc)
target_link_libraries(mw_startup_ring_tests PRIVATE
    Catch2::Catch2WithMain ZLMediaKit::ToolKit)
add_test(NAME mw_startup_ring_tests COMMAND mw_startup_ring_tests)
set_tests_properties(mw_startup_ring_tests PROPERTIES TIMEOUT 30)

add_executable(mw_logging_tests
    logging_test.cc
)

target_include_directories(mw_logging_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/src/streamer"
)

target_link_libraries(mw_logging_tests
    PRIVATE
        Catch2::Catch2WithMain
        FFmpeg::avutil
        ZLMediaKit::ToolKit
        mw::log_test
        mw::streamer_test
)

catch_discover_tests(mw_logging_tests DISCOVERY_MODE PRE_TEST)

add_executable(mw_log_c_api_tests log_c_api_test.c)
target_link_libraries(mw_log_c_api_tests PRIVATE mw::log_test)
set_target_properties(mw_log_c_api_tests PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME mw_log_c_api_tests COMMAND mw_log_c_api_tests)

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

# Separate processes give each singleton pool an independent initial size.
foreach(pool_size IN ITEMS 1 3)
    add_executable(mw_poller_extract_${pool_size}_tests poller_extract_test.cc)
    target_compile_definitions(mw_poller_extract_${pool_size}_tests PRIVATE
        MW_POLLER_TEST_POOL_SIZE=${pool_size})
    target_link_libraries(mw_poller_extract_${pool_size}_tests PRIVATE
        Catch2::Catch2WithMain ZLMediaKit::ToolKit)
    add_test(NAME mw_poller_extract_${pool_size}_tests
        COMMAND mw_poller_extract_${pool_size}_tests)
    set_tests_properties(mw_poller_extract_${pool_size}_tests PROPERTIES TIMEOUT 30)
endforeach()
