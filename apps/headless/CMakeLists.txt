add_executable(ge_headless main.cpp)
target_link_libraries(ge_headless PRIVATE ge_content ge_world)
ge_target_defaults(ge_headless)
set_target_properties(ge_headless PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)

# The CLI supplies --package explicitly, so no source/build path is embedded in
# the executable. A normal build still prepares its documented fixture.
add_dependencies(ge_headless ge_cook_fixture)

install(TARGETS ge_headless
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
)
