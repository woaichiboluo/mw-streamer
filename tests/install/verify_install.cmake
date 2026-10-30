cmake_minimum_required(VERSION 3.22)

function(mw_run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Install verification failed (${result}): ${ARGV}")
    endif()
endfunction()

set(test_root "${MW_BUILD_DIR}/install-test")
file(REMOVE_RECURSE "${test_root}")
# Spaces and relocation catch absolute paths and quoting mistakes in exports.
set(prefix "${test_root}/original prefix")
set(relocated "${test_root}/relocated prefix")
mw_run("${CMAKE_COMMAND}" --install "${MW_BUILD_DIR}"
    --config "${MW_CONFIG}" --prefix "${prefix}")
file(RENAME "${prefix}" "${relocated}")

# The installed SDK must contain exactly the public header tree, never private
# implementation headers from src/.
file(GLOB_RECURSE public_headers RELATIVE "${MW_TEST_SOURCE_DIR}/../../include/mw"
    "${MW_TEST_SOURCE_DIR}/../../include/mw/*.h")
file(GLOB_RECURSE installed_headers RELATIVE "${relocated}/${MW_INSTALL_INCLUDEDIR}/mw"
    "${relocated}/${MW_INSTALL_INCLUDEDIR}/mw/*.h")
if(NOT "${public_headers}" STREQUAL "${installed_headers}")
    message(FATAL_ERROR "Installed headers do not match the public include tree")
endif()

set(generator_options)
if(MW_GENERATOR_PLATFORM)
    list(APPEND generator_options -A "${MW_GENERATOR_PLATFORM}")
endif()
if(MW_GENERATOR_TOOLSET)
    list(APPEND generator_options -T "${MW_GENERATOR_TOOLSET}")
endif()
string(REPLACE ";" "\\;" consumer_prefix_path "${relocated};${MW_PREFIX_PATH}")
mw_run("${CMAKE_COMMAND}" -S "${MW_TEST_SOURCE_DIR}"
    -B "${test_root}/consumer" -G "${MW_GENERATOR}" ${generator_options}
    "-DCMAKE_CXX_COMPILER=${MW_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${MW_CONFIG}"
    "-DCMAKE_PREFIX_PATH=${consumer_prefix_path}"
    "-DFFMPEG_ROOT=${FFMPEG_ROOT}"
    "-DSRT_ROOT=${SRT_ROOT}"
    "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
    "-DMW_TEST_FRAME_CONVTER=${MW_TEST_FRAME_CONVTER}"
    "-DOpenCV_DIR=${OpenCV_DIR}"
    "-DCUDA_TOOLKIT_ROOT_DIR=${CUDA_TOOLKIT_ROOT_DIR}"
    "-DCUDAToolkit_ROOT=${CUDAToolkit_ROOT}")
mw_run("${CMAKE_COMMAND}" --build "${test_root}/consumer" --config "${MW_CONFIG}")
if(WIN32)
    set(ENV{PATH}
        "${relocated}/bin;${FFMPEG_ROOT}/bin;${SRT_ROOT}/bin;${OPENSSL_ROOT_DIR}/bin;$ENV{PATH}")
else()
    # FFmpeg and other external runtime dependencies remain caller-provided.
    set(ENV{LD_LIBRARY_PATH} "${relocated}/lib:${relocated}/lib64:$ENV{LD_LIBRARY_PATH}")
endif()
mw_run("${CMAKE_CTEST_COMMAND}" --test-dir "${test_root}/consumer"
    -C "${MW_CONFIG}" --output-on-failure)
mw_run("${CMAKE_COMMAND}" -S "${MW_TEST_SOURCE_DIR}"
    -B "${test_root}/missing-dependency" -G "${MW_GENERATOR}" ${generator_options}
    "-DCMAKE_CXX_COMPILER=${MW_CXX_COMPILER}"
    "-DCMAKE_PREFIX_PATH=${consumer_prefix_path}"
    -DMW_TEST_MISSING_DEPENDENCY=ON)
