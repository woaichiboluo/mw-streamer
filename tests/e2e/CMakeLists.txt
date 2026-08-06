find_package(CUDAToolkit QUIET)
if(NOT CUDAToolkit_FOUND)
    message(STATUS "Skipping CUDA E2E runner because CUDAToolkit was not found")
    return()
endif()

add_executable(mw_streamer_e2e_runner
    runner/main.cc
)

target_link_libraries(mw_streamer_e2e_runner
    PRIVATE
        CUDA::cuda_driver
        fmt::fmt
        mw::streamer_test
)
