set(MW_STREAMER_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MW_STREAMER_TEST_DATA_DIR "${MW_STREAMER_TEST_ROOT}/data")

add_subdirectory(api)
add_subdirectory(cmake)
add_subdirectory(config)
add_subdirectory(processor)
if(TARGET mw_opencv_adapter)
    add_subdirectory(opencv_adapter)
endif()
add_subdirectory(media)
add_subdirectory(pipeline)
add_subdirectory(io)
add_subdirectory(runtime)
add_subdirectory(e2e)

# These executables exercise Runtime-owned Pipeline or ZLM objects directly.
# Their production counterparts initialize explicitly in the host process.
foreach(target IN ITEMS
    mw_unified_pipeline_config_tests
    mw_pipeline_tests
    mw_pipeline_fatal_tests
    mw_remux_sink_tests
    mw_decoder_sink_tests
    mw_encoder_pipeline_tests
    mw_pipeline_builder_tests
    mw_processor_sink_tests
    mw_sink_message_tests
    mw_input_player_proxy_tests
    mw_pipeline_input_tests
    mw_opencv_adapter_frame_tests)
    if(TARGET ${target})
        target_sources(${target} PRIVATE support/runtime_environment.cc)
    endif()
endforeach()
