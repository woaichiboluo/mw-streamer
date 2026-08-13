find_package(CUDAToolkit REQUIRED)

add_executable(mw_opencv_adapter_frame_tests
    cuda_frame_test.cc
    cuda_mat_adapter_test.cc
    conversion_matrix_test.cc
    host_frame_test.cc
    host_mat_adapter_test.cc
    pipeline_adapter_test.cc
)

target_compile_definitions(mw_opencv_adapter_frame_tests
    PRIVATE
        MW_OPENCV_ADAPTER_TEST_DATA_DIR="${MW_STREAMER_TEST_DATA_DIR}"
)

target_link_libraries(mw_opencv_adapter_frame_tests
    PRIVATE
        Catch2::Catch2WithMain
        CUDA::cuda_driver
        CUDA::cudart
        mw::opencv_adapter
        mw::streamer_test
)

add_test(NAME mw_opencv_adapter_frame_tests COMMAND mw_opencv_adapter_frame_tests)

add_executable(mw_opencv_adapter_initialization_tests
    cuda_initialization_test.cc
)

target_link_libraries(mw_opencv_adapter_initialization_tests
    PRIVATE
        Catch2::Catch2WithMain
        mw::opencv_adapter
)

add_test(
    NAME mw_opencv_adapter_initialization_tests
    COMMAND mw_opencv_adapter_initialization_tests
)
