add_library(ge_core INTERFACE)
add_library(ge::core ALIAS ge_core)
set_target_properties(ge_core PROPERTIES EXPORT_NAME core)

target_compile_features(ge_core INTERFACE cxx_std_20)
target_include_directories(ge_core INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_sources(ge_core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES
        include/ge/core/clock.hpp
        include/ge/core/identity.hpp
)

install(TARGETS ge_core
    EXPORT GameEngineTargets
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development
)
