add_executable(ge_runtime main.cpp modules.cpp)
target_link_libraries(ge_runtime PRIVATE ge_engine)
ge_target_defaults(ge_runtime)
set_target_properties(ge_runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
install(TARGETS ge_runtime RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
