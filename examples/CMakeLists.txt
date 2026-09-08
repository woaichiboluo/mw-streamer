add_executable(mw_remux_sink_example
    remux_sink_example.cc
)

target_link_libraries(mw_remux_sink_example
    PRIVATE mw::streamer
)
