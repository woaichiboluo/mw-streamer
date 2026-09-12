add_subdirectory(log)
add_subdirectory(streamer)

if(BUILD_OPENCV_ADAPTER)
    add_subdirectory(opencv_adapter)
endif()
