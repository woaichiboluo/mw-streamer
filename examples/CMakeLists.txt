add_executable(mw_pipeline_remux_example
    pipeline_remux_example.cc
)

target_link_libraries(mw_pipeline_remux_example
    PRIVATE mw::streamer
)

add_executable(mw_rtsp_publish_example rtsp_publish_example.cc)
target_link_libraries(mw_rtsp_publish_example PRIVATE mw::streamer)
