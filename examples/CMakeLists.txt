add_executable(mw_streamer_c_example
    c_api_example.c
)

target_link_libraries(mw_streamer_c_example
    PRIVATE mw::streamer
)
