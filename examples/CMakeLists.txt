add_executable(mw_remux_pipeline_example remux_pipeline_example.cc)
target_link_libraries(mw_remux_pipeline_example PRIVATE
    mw::streamer CLI11::CLI11)
target_include_directories(mw_remux_pipeline_example SYSTEM PRIVATE
    "$<TARGET_PROPERTY:CLI11::CLI11,INTERFACE_INCLUDE_DIRECTORIES>")
