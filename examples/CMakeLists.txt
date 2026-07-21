add_executable(mw_streamer_version_example version_example.cpp)
target_link_libraries(mw_streamer_version_example PRIVATE mw::streamer fmt::fmt)

add_executable(mw_streamer_transcode_example transcode_example.cpp)
target_link_libraries(
  mw_streamer_transcode_example
  PRIVATE
    mw::streamer
    fmt::fmt
)

find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
  add_executable(
    mw_streamer_cuda_processor_example
    cuda_processor_example.cpp
  )
  target_link_libraries(
    mw_streamer_cuda_processor_example
    PRIVATE
      mw::streamer
      CUDA::cudart
      fmt::fmt
  )
endif()
