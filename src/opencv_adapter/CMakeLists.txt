if(NOT TARGET CUDA::cuda_driver OR NOT TARGET CUDA::cudart)
    find_package(CUDAToolkit REQUIRED)
endif()

find_package(FFmpeg REQUIRED COMPONENTS SWSCALE AVUTIL)

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

set(MW_OPENCV_ADAPTER_SOURCES
    cuda_driver.cc
    cuda_frame.cc
    cuda_mat_adapter.cc
    host_frame.cc
    host_mat_adapter.cc
)

if(BUILD_SHARED)
    add_library(mw_opencv_adapter SHARED ${MW_OPENCV_ADAPTER_SOURCES})
    target_compile_definitions(mw_opencv_adapter
        PRIVATE MW_OPENCV_ADAPTER_BUILDING_LIBRARY
    )
else()
    add_library(mw_opencv_adapter STATIC ${MW_OPENCV_ADAPTER_SOURCES})
    target_compile_definitions(mw_opencv_adapter
        PUBLIC MW_OPENCV_ADAPTER_STATIC_LIBRARY
    )
endif()
add_library(mw::opencv_adapter ALIAS mw_opencv_adapter)

target_include_directories(mw_opencv_adapter
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        ${_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS}
)

target_link_libraries(mw_opencv_adapter
    PUBLIC
        ${_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES}
    PRIVATE
        mw::log
        CUDA::cuda_driver
        CUDA::cudart
        CUDA::nppc
        CUDA::nppial
        CUDA::nppicc
        CUDA::nppidei
        CUDA::nppig
        CUDA::nppitc
        FFmpeg::swscale
        FFmpeg::avutil
)

target_compile_features(mw_opencv_adapter PUBLIC cxx_std_17)
set_target_properties(mw_opencv_adapter PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

unset(_MW_OPENCV_ADAPTER_OPENCV_INCLUDE_DIRS)
unset(_MW_OPENCV_ADAPTER_OPENCV_LIBRARIES)
