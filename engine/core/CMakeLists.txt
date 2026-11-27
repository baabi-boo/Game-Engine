find_package(Threads REQUIRED)
add_library(ge_core STATIC src/binary.cpp src/runtime.cpp src/signals.cpp)
add_library(ge::core ALIAS ge_core)
set_target_properties(ge_core PROPERTIES EXPORT_NAME core)

target_compile_features(ge_core PUBLIC cxx_std_20)
target_link_libraries(ge_core PUBLIC Threads::Threads)
target_include_directories(ge_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_sources(ge_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES
        include/ge/core/clock.hpp
        include/ge/core/identity.hpp
        include/ge/core/binary.hpp
        include/ge/core/memory.hpp
        include/ge/core/runtime.hpp
        include/ge/core/signals.hpp
)
set_target_properties(ge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
ge_target_defaults(ge_core)

install(TARGETS ge_core
    EXPORT GameEngineTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT Development
)
