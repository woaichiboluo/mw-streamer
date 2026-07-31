find_package(Threads REQUIRED)

add_library(hdr_histogram STATIC
    src/hdr_histogram.c
    src/hdr_interval_recorder.c
    src/hdr_thread.c
    src/hdr_writer_reader_phaser.c
)
add_library(HdrHistogram::HdrHistogram ALIAS hdr_histogram)

set_target_properties(hdr_histogram
    PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

target_include_directories(hdr_histogram
    SYSTEM PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(hdr_histogram PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(hdr_histogram PRIVATE ws2_32)
elseif(UNIX)
    target_compile_definitions(hdr_histogram PRIVATE _GNU_SOURCE)
    target_link_libraries(hdr_histogram PRIVATE m)
endif()
