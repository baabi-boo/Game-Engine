cmake_minimum_required(VERSION 3.25)

project(GameEngine
    VERSION 0.1.0
    DESCRIPTION "Independent headless engine foundation"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are unsupported. Use cmake --preset dev; build products belong in out/."
    )
endif()

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)
include(cmake/ProjectOptions.cmake)

# Cooking uses only Python's standard library. Nothing in this build downloads
# dependencies or searches for an undeclared third-party package.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(GE_AUTHORED_FIXTURE "${PROJECT_SOURCE_DIR}/assets/fixtures/district.world.json")
set(GE_COOKED_FIXTURE "${PROJECT_BINARY_DIR}/assets/district.gepack")
set(GE_INSTALL_ASSETDIR "${CMAKE_INSTALL_DATADIR}/game-engine/assets")

add_custom_command(
    OUTPUT "${GE_COOKED_FIXTURE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/assets"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/cook.py"
        "${GE_AUTHORED_FIXTURE}" --output "${GE_COOKED_FIXTURE}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/cook.py"
        "${GE_AUTHORED_FIXTURE}"
    COMMENT "Validating and cooking the synthetic district fixture"
    VERBATIM
)
add_custom_target(ge_cook_fixture DEPENDS "${GE_COOKED_FIXTURE}")

add_subdirectory(engine/core)
add_subdirectory(engine/content)
add_subdirectory(engine/world)
add_subdirectory(apps/headless)

if(BUILD_TESTING)
    # A requested test suite must exist: do not silently skip a missing directory.
    add_subdirectory(tests)
endif()

install(FILES "${GE_COOKED_FIXTURE}" "${GE_AUTHORED_FIXTURE}"
    DESTINATION "${GE_INSTALL_ASSETDIR}"
    COMPONENT Runtime
)
install(FILES "${PROJECT_SOURCE_DIR}/cmake/DistributionReadme.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME README.md
    COMPONENT Documentation
)
install(FILES "${PROJECT_SOURCE_DIR}/NOTICE.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT Documentation
)
# Keep NOTICE's policy and research destinations available in the archive.
# Policy links about contributing refer to the source repository, rather than
# pretending that source-only build/test documents are part of the binary SDK.
file(READ "${PROJECT_SOURCE_DIR}/docs/project-policy.md" GE_DISTRIBUTION_POLICY)
set(GE_SOURCE_DOCUMENTS "https://github.com/baabi-boo/Game-Engine/blob/main")
string(REPLACE "](architecture/overview.md)" "](${GE_SOURCE_DOCUMENTS}/docs/architecture/overview.md)"
    GE_DISTRIBUTION_POLICY "${GE_DISTRIBUTION_POLICY}")
string(REPLACE "](quality-gates.md)" "](${GE_SOURCE_DOCUMENTS}/docs/quality-gates.md)"
    GE_DISTRIBUTION_POLICY "${GE_DISTRIBUTION_POLICY}")
string(REPLACE "](../SECURITY.md)" "](${GE_SOURCE_DOCUMENTS}/SECURITY.md)"
    GE_DISTRIBUTION_POLICY "${GE_DISTRIBUTION_POLICY}")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/cmake/distribution/project-policy.md"
    CONTENT "${GE_DISTRIBUTION_POLICY}" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/cmake/distribution/project-policy.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
    COMPONENT Documentation
)
install(FILES "${PROJECT_SOURCE_DIR}/docs/research/open-world-blueprint.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/research"
    COMPONENT Documentation
)

set(GE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/GameEngine")
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/GameEngineConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/GameEngineConfig.cmake"
    INSTALL_DESTINATION "${GE_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/cmake/GameEngineConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)
install(EXPORT GameEngineTargets
    FILE GameEngineTargets.cmake
    NAMESPACE ge::
    DESTINATION "${GE_INSTALL_CMAKEDIR}"
    COMPONENT Development
)
install(FILES
    "${PROJECT_BINARY_DIR}/cmake/GameEngineConfig.cmake"
    "${PROJECT_BINARY_DIR}/cmake/GameEngineConfigVersion.cmake"
    DESTINATION "${GE_INSTALL_CMAKEDIR}"
    COMPONENT Development
)

if(GE_ENABLE_PACKAGING)
    include(cmake/Packaging.cmake)
endif()
