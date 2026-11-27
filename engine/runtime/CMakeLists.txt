include("${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake")
find_package(Threads REQUIRED)

add_library(ge_engine STATIC ${GE_ENGINE_SOURCES})
add_library(ge::engine ALIAS ge_engine)
set_target_properties(ge_engine PROPERTIES
    EXPORT_NAME engine
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(ge_engine PUBLIC ge_core ge_content ge_world Threads::Threads)
target_compile_features(ge_engine PUBLIC cxx_std_20)
foreach(module IN LISTS GE_ENGINE_MODULES)
    target_include_directories(ge_engine PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/engine/${module}/include>"
    )
endforeach()
target_include_directories(ge_engine PUBLIC
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_sources(ge_engine PUBLIC FILE_SET HEADERS
    BASE_DIRS ${GE_ENGINE_INCLUDE_ROOTS}
    FILES ${GE_ENGINE_HEADERS}
)
ge_target_defaults(ge_engine)
if(WIN32)
    target_link_libraries(ge_engine PRIVATE ws2_32)
endif()
install(TARGETS ge_engine
    EXPORT GameEngineTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development
)
