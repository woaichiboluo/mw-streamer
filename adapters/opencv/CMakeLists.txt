get_filename_component(_MW_STREAMER_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    ABSOLUTE
)

if(NOT TARGET CUDA::cuda_driver OR NOT TARGET CUDA::cudart)
    find_package(CUDAToolkit REQUIRED)
endif()

if(TARGET FFmpeg::swscale)
    set(_MW_OPENCV_ADAPTER_SWSCALE FFmpeg::swscale)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MW_OPENCV_ADAPTER_SWSCALE
        REQUIRED
        IMPORTED_TARGET
        libswscale
    )
    set(_MW_OPENCV_ADAPTER_SWSCALE PkgConfig::MW_OPENCV_ADAPTER_SWSCALE)
endif()

find_package(OpenCV QUIET COMPONENTS core cudaarithm)
if(OpenCV_FOUND)
    set(_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS ${OpenCV_INCLUDE_DIRS})
    set(_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES ${OpenCV_LIBS})
else()
    find_path(MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIR
        NAMES opencv2/core/mat.hpp
        PATH_SUFFIXES opencv4
    )
    find_library(MW_OPENCV_ADAPTER_OPENCV_CORE_LIBRARY
        NAMES opencv_core
    )
    find_library(MW_OPENCV_ADAPTER_OPENCV_CUDAARITHM_LIBRARY
        NAMES opencv_cudaarithm
    )
    if(NOT MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIR OR
       NOT MW_OPENCV_ADAPTER_OPENCV_CORE_LIBRARY OR
       NOT MW_OPENCV_ADAPTER_OPENCV_CUDAARITHM_LIBRARY)
        message(FATAL_ERROR
            "mw_opencv_adapter requires an OpenCV build with CUDA support"
        )
    endif()
    set(_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS
        ${MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIR}
    )
    set(_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES
        ${MW_OPENCV_ADAPTER_OPENCV_CORE_LIBRARY}
        ${MW_OPENCV_ADAPTER_OPENCV_CUDAARITHM_LIBRARY}
    )
endif()

add_library(mw_opencv_adapter STATIC
    src/cuda_driver.cc
    src/cuda_frame.cc
    src/cuda_mat_adapter.cc
    src/host_frame.cc
    src/host_mat_adapter.cc
)
add_library(mw::opencv_adapter ALIAS mw_opencv_adapter)

target_include_directories(mw_opencv_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${_MW_STREAMER_ROOT}/include>"
        ${_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS}
)

target_link_libraries(mw_opencv_adapter
    PUBLIC
        ${_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES}
    PRIVATE
        CUDA::cuda_driver
        CUDA::cudart
        CUDA::nppc
        CUDA::nppial
        CUDA::nppicc
        CUDA::nppidei
        CUDA::nppig
        CUDA::nppitc
        ${_MW_OPENCV_ADAPTER_SWSCALE}
)

target_compile_features(mw_opencv_adapter PUBLIC cxx_std_17)
set_target_properties(mw_opencv_adapter PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

unset(_MW_STREAMER_ROOT)
unset(_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS)
unset(_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES)
unset(_MW_OPENCV_ADAPTER_SWSCALE)
