add_library(ge_content STATIC src/package.cpp)
add_library(ge::content ALIAS ge_content)
set_target_properties(ge_content PROPERTIES
    EXPORT_NAME content
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(ge_content PUBLIC ge_core)
target_compile_features(ge_content PUBLIC cxx_std_20)
target_include_directories(ge_content PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_sources(ge_content PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES include/ge/content/package.hpp
)
ge_target_defaults(ge_content)

install(TARGETS ge_content
    EXPORT GameEngineTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development
)
