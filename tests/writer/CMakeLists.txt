add_executable(mw_video_writer_tests video_writer_test.cc)

target_link_libraries(mw_video_writer_tests PRIVATE
    Catch2::Catch2WithMain mw::streamer FFmpeg::avformat FFmpeg::avcodec)
add_test(NAME mw_video_writer_tests COMMAND mw_video_writer_tests)
set_tests_properties(mw_video_writer_tests PROPERTIES
    TIMEOUT 60 LABELS "writer;integration")
mw_copy_test_runtime_dlls(mw_video_writer_tests)
