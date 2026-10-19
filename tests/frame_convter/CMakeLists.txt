find_package(CUDAToolkit REQUIRED)

add_executable(mw_frame_convter_tests
    opencv_cpu_convter_test.cc cpu_frame_to_cuda_test.cc opencv_cuda_convter_test.cc)
target_link_libraries(mw_frame_convter_tests PRIVATE
    mw::frame_convter Catch2::Catch2WithMain CUDA::cuda_driver)
add_test(NAME mw_frame_convter_tests COMMAND mw_frame_convter_tests)
set_tests_properties(mw_frame_convter_tests PROPERTIES TIMEOUT 60)
mw_copy_test_runtime_dlls(mw_frame_convter_tests)
