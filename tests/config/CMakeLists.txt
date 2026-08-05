add_executable(mw_decoder_config_tests
    decoder_config_test.cc
)

target_include_directories(mw_decoder_config_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_decoder_config_tests
    PRIVATE Catch2::Catch2WithMain
)

add_test(NAME mw_decoder_config_tests COMMAND mw_decoder_config_tests)

add_executable(mw_encoder_config_tests
    encoder_config_test.cc
)

target_include_directories(mw_encoder_config_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_encoder_config_tests
    PRIVATE Catch2::Catch2WithMain
)

add_test(NAME mw_encoder_config_tests COMMAND mw_encoder_config_tests)

add_executable(mw_pipeline_config_tests
    pipeline_config_test.cc
)

target_include_directories(mw_pipeline_config_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(mw_pipeline_config_tests
    PRIVATE Catch2::Catch2WithMain
)

add_test(NAME mw_pipeline_config_tests COMMAND mw_pipeline_config_tests)

add_executable(mw_toml_config_tests
    toml_config_test.cc
)

target_compile_definitions(mw_toml_config_tests
    PRIVATE
        MW_TOML_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/template"
)

target_link_libraries(mw_toml_config_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::streamer_test
)

add_test(NAME mw_toml_config_tests COMMAND mw_toml_config_tests)
