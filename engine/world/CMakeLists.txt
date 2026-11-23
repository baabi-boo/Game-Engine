add_library(ge_world STATIC
    src/streaming.cpp
    src/state.cpp
)
add_library(ge::world ALIAS ge_world)
set_target_properties(ge_world PROPERTIES
    EXPORT_NAME world
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(ge_world PUBLIC ge_core)
target_compile_features(ge_world PUBLIC cxx_std_20)
target_include_directories(ge_world PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_sources(ge_world PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES
        include/ge/world/streaming.hpp
        include/ge/world/state.hpp
)
ge_target_defaults(ge_world)

install(TARGETS ge_world
    EXPORT GameEngineTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development
)
