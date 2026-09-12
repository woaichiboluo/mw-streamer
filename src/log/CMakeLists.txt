if(BUILD_SHARED)
    add_library(mw_log SHARED logging.cc)
    target_compile_definitions(mw_log PRIVATE MW_LOG_BUILDING_LIBRARY)
else()
    add_library(mw_log STATIC logging.cc)
    target_compile_definitions(mw_log PUBLIC MW_LOG_STATIC_LIBRARY)
endif()
add_library(mw::log ALIAS mw_log)

target_include_directories(mw_log
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(mw_log
    PUBLIC
        fmt::fmt
    PRIVATE
        spdlog::spdlog
)

target_compile_features(mw_log PUBLIC cxx_std_17)
set_target_properties(mw_log PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
