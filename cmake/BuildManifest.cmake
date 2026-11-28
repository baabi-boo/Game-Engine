include_guard(GLOBAL)

# Each configuration hashes its own completed executables and SDK libraries.
# An ALL target reruns on normal builds to observe changes to the Git working tree.
# It depends on the shipped artifacts; none depends on the manifest.
set(GE_BUILD_MANIFEST "${PROJECT_BINARY_DIR}/build-manifest/$<CONFIG>/build-manifest.json")
add_custom_target(ge_build_manifest ALL
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build_manifest.py"
        --source-dir "${PROJECT_SOURCE_DIR}"
        --binary "$<TARGET_FILE:ge_headless>"
        --runtime "$<TARGET_FILE:ge_runtime>"
        --engine-library "$<TARGET_FILE:ge_engine>"
        --core-library "$<TARGET_FILE:ge_core>"
        --content-library "$<TARGET_FILE:ge_content>"
        --world-library "$<TARGET_FILE:ge_world>"
        --fixture "${GE_COOKED_FIXTURE}"
        --output "${GE_BUILD_MANIFEST}"
        --project-name "${PROJECT_NAME}"
        --project-version "${PROJECT_VERSION}"
        --platform "${CMAKE_SYSTEM_NAME}"
        --system-processor "${CMAKE_SYSTEM_PROCESSOR}"
        --compiler-id "${CMAKE_CXX_COMPILER_ID}"
        --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
        --configuration "$<CONFIG>"
        --strict-warnings "$<BOOL:${GE_STRICT_WARNINGS}>"
        --sanitizers "$<BOOL:${GE_ENABLE_SANITIZERS}>"
    DEPENDS ge_headless ge_runtime ge_engine ge_core ge_content ge_world ge_cook_fixture
    BYPRODUCTS "${GE_BUILD_MANIFEST}"
    COMMENT "Recording build settings and hashes of the completed artifacts"
    VERBATIM
)

install(FILES "${GE_BUILD_MANIFEST}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/game-engine"
    COMPONENT Runtime
)
