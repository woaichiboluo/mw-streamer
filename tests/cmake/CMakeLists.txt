set(_MW_FIND_SRT_TEST_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/find_srt_test.cmake"
)

foreach(_case IN ITEMS
        linkage_required
        linux_shared
        linux_static
        windows_shared
        windows_static
        windows_static_pkg_config
)
    add_test(
        NAME mw_find_srt_${_case}_test
        COMMAND ${CMAKE_COMMAND}
            -DCASE=${_case}
            -DCMAKE_EXECUTABLE=${CMAKE_COMMAND}
            -DFIND_SRT_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake
            -DFIXTURE_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/find_srt
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/${_case}
            -P ${_MW_FIND_SRT_TEST_SCRIPT}
    )
endforeach()

unset(_MW_FIND_SRT_TEST_SCRIPT)

set(_MW_FIND_FFMPEG_TEST_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/find_ffmpeg_test.cmake"
)

foreach(_case IN ITEMS
        linkage_required
        pkg_config_required
        linux_shared
        linux_static
        windows_shared
        windows_static
)
    add_test(
        NAME mw_find_ffmpeg_${_case}_test
        COMMAND ${CMAKE_COMMAND}
            -DCASE=${_case}
            -DCMAKE_EXECUTABLE=${CMAKE_COMMAND}
            -DFIND_FFMPEG_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake
            -DFIXTURE_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/find_ffmpeg
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_${_case}
            -P ${_MW_FIND_FFMPEG_TEST_SCRIPT}
    )
endforeach()

unset(_MW_FIND_FFMPEG_TEST_SCRIPT)
