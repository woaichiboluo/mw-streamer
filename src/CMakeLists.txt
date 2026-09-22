if(NOT TARGET mw_log)
    add_subdirectory(log)
endif()
add_subdirectory(streamer)

if(BUILD_OPENCV_ADAPTER)
    add_subdirectory(opencv_adapter)
endif()
