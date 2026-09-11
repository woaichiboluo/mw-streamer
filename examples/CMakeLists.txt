add_executable(mw_pipeline_remux_example
    pipeline_remux_example.cc
)

target_link_libraries(mw_pipeline_remux_example
    PRIVATE mw::streamer
)
