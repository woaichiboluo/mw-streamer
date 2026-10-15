add_executable(mw_audio_resampler_tests audio_resampler_test.cc)
target_link_libraries(mw_audio_resampler_tests PRIVATE
    mw::streamer Catch2::Catch2WithMain FFmpeg::swresample
)
add_test(NAME mw_audio_resampler_tests COMMAND mw_audio_resampler_tests)
set_tests_properties(mw_audio_resampler_tests PROPERTIES TIMEOUT 30)
mw_copy_test_runtime_dlls(mw_audio_resampler_tests)
